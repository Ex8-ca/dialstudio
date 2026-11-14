#include "protocol.h"

#include "teacodec.h"

namespace Protocol {

QString keyName(int key) { return QStringLiteral("key%1").arg(key); }

int keyFromWireId(quint8 id) {
    if (id >= 0x01 && id <= 0x06)
        return id;
    if (id == 0x08)
        return 7;
    return 0;
}

quint8 wireIdFromKey(int key) {
    if (key >= 1 && key <= 6)
        return quint8(key);
    return key == 7 ? 0x08 : 0;
}

namespace {

bool hasPrefix(const QByteArray &p, std::initializer_list<int> prefix) {
    int i = 0;
    for (int b : prefix) {
        if (quint8(p[i++]) != b)
            return false;
    }
    return true;
}

QByteArray command(std::initializer_list<int> bytes) {
    QByteArray plain;
    for (int b : bytes)
        plain.append(char(b));
    return TeaCodec::buildPacket(plain);
}

} // namespace

Message parsePlain(const QByteArray &p) {
    Message m;
    m.raw = p;
    if (p.size() != TeaCodec::PacketSize)
        return m;

    if (quint8(p[0]) == 0x8b && quint8(p[1]) == 0x10) {
        const quint8 id = quint8(p[2]);
        const bool down = quint8(p[3]) == 0x01;
        if (const int key = keyFromWireId(id)) {
            m.kind = down ? Message::Kind::KeyDown : Message::Kind::KeyUp;
            m.key = key;
        } else if (id == 0x07) {
            // A synthetic down+up pair arrives on release; only the down counts.
            m.kind = down ? Message::Kind::KnobClick : Message::Kind::Ignored;
        } else if (id >= 0x09 && id <= 0x0c) {
            m.kind = Message::Kind::Rotate;
            m.pressed = id >= 0x0b;
            m.direction = (id == 0x0a || id == 0x0c) ? 1 : -1;
        } else if (id >= 0x41 && id <= 0x44) {
            m.kind = Message::Kind::Offline;
            m.key = id - 0x40;
        }
        return m;
    }

    if (hasPrefix(p, {0x06, 0x01, 0x23, 0x10})) {
        m.kind = Message::Kind::Heartbeat;
    } else if (hasPrefix(p, {0x01, 0x06, 0x40}) && (quint8(p[3]) == 0x11 || quint8(p[3]) == 0x14)) {
        m.kind = Message::Kind::Haptics;
        m.writeAck = quint8(p[3]) == 0x14;
        m.on = quint8(p[4]) != 0;
    } else if (hasPrefix(p, {0x01, 0x01, 0x42}) && (quint8(p[3]) == 0x11 || quint8(p[3]) == 0x12)) {
        m.kind = Message::Kind::Sleep;
        m.writeAck = quint8(p[3]) == 0x12;
        m.seconds = quint8(p[4]) | (quint8(p[5]) << 8);
    } else if (hasPrefix(p, {0x01, 0x04, 0x04, 0x11})) {
        m.kind = Message::Kind::Firmware;
        m.customCode = QStringLiteral("%1%2%3")
                           .arg(quint8(p[4]), 2, 16, QChar('0'))
                           .arg(quint8(p[5]), 2, 16, QChar('0'))
                           .arg(quint8(p[6]), 2, 16, QChar('0'))
                           .toUpper();
        m.text = QStringLiteral("%1.%2.%3").arg(quint8(p[10])).arg(quint8(p[11])).arg(quint8(p[12]));
        if (quint8(p[18]) != 0)
            m.text += QStringLiteral(".%1").arg(quint8(p[18]));
    } else if (hasPrefix(p, {0x01, 0x01, 0x0b, 0x11})) {
        m.kind = Message::Kind::SerialFragment;
        const int length = qMin<int>(quint8(p[4]), TeaCodec::PacketSize - 6);
        m.index = quint8(p[5]);
        m.text = QString::fromLatin1(p.mid(6, length));
    } else if (hasPrefix(p, {0x0b, 0x7b})) {
        m.kind = Message::Kind::Battery;
        m.millivolts = quint8(p[2]) | (quint8(p[3]) << 8);
        const quint8 flags = quint8(p[4]);
        m.lowBattery = flags & 0x03;
        m.powerOff = flags & 0x04;
        m.charging = flags & 0x08;
        m.full = flags & 0x10;
        m.percent = quint8(p[5]);
    }
    return m;
}

Message parse(const QByteArray &encrypted) {
    if (encrypted.size() != TeaCodec::PacketSize) {
        Message m;
        m.raw = encrypted;
        return m;
    }
    return parsePlain(TeaCodec::decrypt(encrypted));
}

QByteArray heartbeat() { return command({0x06, 0x01, 0x23, 0x00, 0x01}); }
QByteArray readHaptics() { return command({0x01, 0x06, 0x40, 0x01}); }
QByteArray setHaptics(bool on) { return command({0x01, 0x06, 0x40, 0x04, on ? 0xff : 0x00}); }
QByteArray readSleep() { return command({0x01, 0x01, 0x42, 0x01}); }
QByteArray setSleep(int seconds) {
    const int s = qBound(0, seconds, 0xffff);
    return command({0x01, 0x01, 0x42, 0x02, s & 0xff, (s >> 8) & 0xff});
}
QByteArray readFirmware() { return command({0x01, 0x04, 0x04, 0x01}); }
QByteArray readSerial() { return command({0x01, 0x01, 0x0b, 0x01}); }

QString describe(const Message &m) {
    using K = Message::Kind;
    switch (m.kind) {
    case K::KeyDown: return keyName(m.key) + " press";
    case K::KeyUp: return keyName(m.key) + " release";
    case K::KnobClick: return QStringLiteral("knob click");
    case K::Rotate:
        return QStringLiteral("%1 %2").arg(m.pressed ? "press_rotate" : "rotate", m.direction > 0 ? "cw" : "ccw");
    default: return QStringLiteral("other");
    }
}

} // namespace Protocol
