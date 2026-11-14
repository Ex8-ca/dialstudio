#include <QtTest>

#include "protocol.h"
#include "teacodec.h"

namespace {
QByteArray hex(const char *s) { return QByteArray::fromHex(s); }
QByteArray padded(const char *prefix) {
    QByteArray b = hex(prefix);
    return b + QByteArray(20 - b.size(), '\0');
}
}

class CodecTests : public QObject {
    Q_OBJECT
private slots:
    void vectors_data() {
        QTest::addColumn<QByteArray>("plain");
        QTest::addColumn<QByteArray>("cipher");
        QTest::newRow("top-left down") << padded("8b100101") << hex("8e8f5e16ec67281e3890c499a360aaad00000000");
        QTest::newRow("top-left up") << padded("8b100100") << hex("028db3660224d0c73890c499a360aaad00000000");
        QTest::newRow("host heartbeat") << padded("0601230001") << hex("24569ef228e145a13890c499a360aaad00000000");
        QTest::newRow("heartbeat reply") << padded("0601231001") << hex("21f45eb71c3587ab3890c499a360aaad00000000");
    }
    void vectors() {
        QFETCH(QByteArray, plain);
        QFETCH(QByteArray, cipher);
        QCOMPARE(TeaCodec::encrypt(plain), cipher);
        QCOMPARE(TeaCodec::decrypt(cipher), plain);
    }
    void tailPassesThrough() {
        QByteArray p = padded("8b100101");
        p[16] = char(0xde); p[19] = char(0xad);
        const QByteArray c = TeaCodec::encrypt(p);
        QCOMPARE(c.mid(16), p.mid(16));
        QCOMPARE(TeaCodec::decrypt(c), p);
    }
    void zeroBlockCiphertext() {
        QCOMPARE(TeaCodec::encrypt(QByteArray(8, '\0')), hex("3890c499a360aaad"));
    }
    void buildPacketPads() {
        QCOMPARE(TeaCodec::buildPacket(hex("8b100101")), hex("8e8f5e16ec67281e3890c499a360aaad00000000"));
        QCOMPARE(TeaCodec::buildPacket(hex("0601230001")).size(), 20);
    }
    void keyBytes() { QCOMPARE(TeaCodec::key(), hex("cabaa5ca6d8a2abcba9e5acaca8bb89b")); }
};

class ProtocolTests : public QObject {
    Q_OBJECT
    using K = Protocol::Message::Kind;
private slots:
    void captured_replies() {
        // The four replies captured in PLAN.md section 1 (decrypted forms).
        auto hb = Protocol::parsePlain(hex("0601231001000000000000000000000000000000"));
        QCOMPARE(hb.kind, K::Heartbeat);

        auto fw = Protocol::parsePlain(hex("0104041140003728280101030826040818000100"));
        QCOMPARE(fw.kind, K::Firmware);
        QCOMPARE(fw.text, QString("1.3.8.1"));
        QCOMPARE(fw.customCode, QString("400037"));

        auto h = Protocol::parsePlain(hex("01064011ff000000000000000000000000000000"));
        QCOMPARE(h.kind, K::Haptics);
        QVERIFY(h.on);
        QVERIFY(!h.writeAck);

        auto s = Protocol::parsePlain(hex("01014211201c0000000000000000000000000000"));
        QCOMPARE(s.kind, K::Sleep);
        QCOMPARE(s.seconds, 7200);
    }
    void captured_replies_encrypted() {
        auto hb = Protocol::parse(hex("21f45eb71c3587ab3890c499a360aaad00000000"));
        QCOMPARE(hb.kind, K::Heartbeat);
        auto fw = Protocol::parse(TeaCodec::encrypt(hex("0104041140003728280101030826040818000100")));
        QCOMPARE(fw.text, QString("1.3.8.1"));
    }
    void firmwareWithoutFourthPart() {
        auto fw = Protocol::parsePlain(hex("0104041140003728280101030826040818000000"));
        QCOMPARE(fw.text, QString("1.3.8"));
    }
    void keyEvents_data() {
        QTest::addColumn<QString>("id");
        QTest::addColumn<int>("key");
        QTest::newRow("01") << "01" << 1;
        QTest::newRow("02") << "02" << 2;
        QTest::newRow("03") << "03" << 3;
        QTest::newRow("04") << "04" << 4;
        QTest::newRow("05") << "05" << 5;
        QTest::newRow("06") << "06" << 6;
        QTest::newRow("08") << "08" << 7;
    }
    void keyEvents() {
        QFETCH(QString, id);
        QFETCH(int, key);
        auto down = Protocol::parsePlain(padded(("8b10" + id + "01").toLatin1()));
        QCOMPARE(down.kind, K::KeyDown);
        QCOMPARE(down.key, key);
        auto up = Protocol::parsePlain(padded(("8b10" + id + "00").toLatin1()));
        QCOMPARE(up.kind, K::KeyUp);
        QCOMPARE(up.key, key);
    }
    void knobClickOnlyOnDown() {
        QCOMPARE(Protocol::parsePlain(padded("8b100701")).kind, K::KnobClick);
        QCOMPARE(Protocol::parsePlain(padded("8b100700")).kind, K::Ignored);
    }
    void rotation_data() {
        QTest::addColumn<QString>("id");
        QTest::addColumn<int>("dir");
        QTest::addColumn<bool>("pressed");
        QTest::newRow("ccw") << "09" << -1 << false;
        QTest::newRow("cw") << "0a" << 1 << false;
        QTest::newRow("ccw pressed") << "0b" << -1 << true;
        QTest::newRow("cw pressed") << "0c" << 1 << true;
    }
    void rotation() {
        QFETCH(QString, id);
        QFETCH(int, dir);
        QFETCH(bool, pressed);
        auto m = Protocol::parsePlain(padded(("8b10" + id + "01").toLatin1()));
        QCOMPARE(m.kind, K::Rotate);
        QCOMPARE(m.direction, dir);
        QCOMPARE(m.pressed, pressed);
    }
    void offlineIds() {
        for (const char *id : {"41", "42", "43", "44"})
            QCOMPARE(Protocol::parsePlain(padded((QByteArray("8b10") + id + "01").constData())).kind, K::Offline);
    }
    void battery() {
        auto b = Protocol::parsePlain(padded("0b7b22100064"));
        QCOMPARE(b.kind, K::Battery);
        QCOMPARE(b.millivolts, 4130);
        QCOMPARE(b.percent, 100);
        QVERIFY(!b.charging);
        auto c = Protocol::parsePlain(padded("0b7b1c100864"));
        QVERIFY(c.charging);
        QVERIFY(!c.full);
        auto f = Protocol::parsePlain(padded("0b7b1c101864"));
        QVERIFY(f.full);
        auto l = Protocol::parsePlain(padded("0b7b1c100f10"));
        QCOMPARE(l.lowBattery, 3);
        QVERIFY(l.powerOff);
        QVERIFY(l.charging);
    }
    void serialFragments() {
        auto a = Protocol::parsePlain(hex("01010b110a004130443236493030335500000000"));
        auto b = Protocol::parsePlain(hex("01010b11070133373131313137000000000000"
                                         "00"));
        QCOMPARE(a.kind, K::SerialFragment);
        QCOMPARE(a.index, 0);
        QCOMPARE(a.text, QString("A0D26I003U"));
        QCOMPARE(b.index, 1);
        QCOMPARE(b.text, QString("3711117"));
    }
    void settingsAcks() {
        auto h = Protocol::parsePlain(padded("01064014" "00"));
        QCOMPARE(h.kind, K::Haptics);
        QVERIFY(h.writeAck);
        QVERIFY(!h.on);
        auto s = Protocol::parsePlain(padded("01014212100e"));
        QCOMPARE(s.kind, K::Sleep);
        QCOMPARE(s.seconds, 3600);
        QVERIFY(s.writeAck);
    }
    void unknownIsNotAnError() {
        QCOMPARE(Protocol::parsePlain(padded("ffee0102")).kind, K::Unknown);
        QCOMPARE(Protocol::parse(QByteArray(5, 'x')).kind, K::Unknown);
    }
    void commandBuilders() {
        QCOMPARE(Protocol::heartbeat(), hex("24569ef228e145a13890c499a360aaad00000000"));
        QCOMPARE(TeaCodec::decrypt(Protocol::readHaptics()), padded("01064001"));
        QCOMPARE(TeaCodec::decrypt(Protocol::setHaptics(true)), padded("01064004ff"));
        QCOMPARE(TeaCodec::decrypt(Protocol::setHaptics(false)), padded("0106400400"));
        QCOMPARE(TeaCodec::decrypt(Protocol::readSleep()), padded("01014201"));
        QCOMPARE(TeaCodec::decrypt(Protocol::setSleep(7200)), padded("01014202201c"));
        QCOMPARE(TeaCodec::decrypt(Protocol::setSleep(3600)), padded("01014202100e"));
        QCOMPARE(TeaCodec::decrypt(Protocol::readFirmware()), padded("01040401"));
        QCOMPARE(TeaCodec::decrypt(Protocol::readSerial()), padded("01010b01"));
    }
    void describeStrings() {
        QCOMPARE(Protocol::describe(Protocol::parsePlain(padded("8b100801"))), QString("key7 press"));
        QCOMPARE(Protocol::describe(Protocol::parsePlain(padded("8b100c01"))), QString("press_rotate cw"));
    }
};

int runAll(int argc, char **argv) {
    int status = 0;
    { CodecTests t; status |= QTest::qExec(&t, argc, argv); }
    { ProtocolTests t; status |= QTest::qExec(&t, argc, argv); }
    return status;
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    return runAll(argc, argv);
}

#include "dialstudio_tests.moc"
