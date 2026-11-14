#include "testmain.h"

#include <QDBusConnection>
#include <QSignalSpy>

#include "bluezlink.h"
#include "fakebluez.h"

// These run against FakeBluez on the private session bus that bin/test creates with
// dbus-run-session; they refuse to run on any other bus.
class BluezLinkTests : public QObject {
    Q_OBJECT

    static bool onPrivateBus() { return qEnvironmentVariableIsSet("DIALSTUDIO_TEST_BUS"); }

    static void waitState(BluezLink &link, BluezLink::State s, int ms = 3000) {
        QTRY_COMPARE_WITH_TIMEOUT(link.state(), s, ms);
    }

private slots:
    void initTestCase() {
        if (!onPrivateBus()) QSKIP("needs the private session bus from bin/test (dbus-run-session)");
    }

    void modelFindsDeviceByVendorServiceNotByName() {
        BluezModel m;
        BluezObjects objects;
        const QDBusObjectPath dev("/d/dev_1"), svc("/d/dev_1/service1"), other("/d/dev_2");
        objects[dev]["org.bluez.Device1"] = {{"Address", "AA:AA:AA:AA:AA:01"}, {"Name", "Totally Renamed"}, {"Connected", false}};
        objects[svc]["org.bluez.GattService1"] = {{"UUID", BluezUuid::Service.toUpper()}, {"Device", QVariant::fromValue(dev)}};
        objects[other]["org.bluez.Device1"] = {{"Address", "AA:AA:AA:AA:AA:02"}, {"Name", "Ulanzi Dial copy"}};
        m.replaceAll(objects);
        QCOMPARE(m.findDevice(""), QString("/d/dev_1"));
        QCOMPARE(m.findDevice("aa:aa:aa:aa:aa:02"), QString("/d/dev_2"));     // name fallback, address filter
        QCOMPARE(m.findDevice("00:00:00:00:00:00"), QString());
    }

    void modelResolvesCharacteristicsByUuidAnywhereInTheTree() {
        BluezModel m;
        BluezObjects o;
        const QDBusObjectPath dev("/org/bluez/hci1/dev_X"), svc("/org/bluez/hci1/dev_X/service0099");
        o[dev]["org.bluez.Device1"] = {{"Address", "AA:AA:AA:AA:AA:01"}, {"Adapter", QVariant::fromValue(QDBusObjectPath("/org/bluez/hci1"))}};
        o[svc]["org.bluez.GattService1"] = {{"UUID", BluezUuid::Service}, {"Device", QVariant::fromValue(dev)}};
        // handles differ from the dial in the plan, and the write char comes first
        o[QDBusObjectPath(svc.path() + "/char0abc")]["org.bluez.GattCharacteristic1"] = {{"UUID", BluezUuid::Write}, {"Service", QVariant::fromValue(svc)}};
        o[QDBusObjectPath(svc.path() + "/char0abd")]["org.bluez.GattCharacteristic1"] = {{"UUID", BluezUuid::Notify}, {"Service", QVariant::fromValue(svc)}};
        // a same-UUID characteristic under a different service must be ignored
        o[QDBusObjectPath("/elsewhere/char")]["org.bluez.GattCharacteristic1"] = {{"UUID", BluezUuid::Notify}, {"Service", QVariant::fromValue(QDBusObjectPath("/elsewhere"))}};
        m.replaceAll(o);
        QCOMPARE(m.characteristic(dev.path(), BluezUuid::Notify), svc.path() + "/char0abd");
        QCOMPARE(m.characteristic(dev.path(), BluezUuid::Write), svc.path() + "/char0abc");
        QCOMPARE(m.adapterOf(dev.path()), QString("/org/bluez/hci1"));
    }

    void heartbeatStartsImmediatelyThenEverySecond() {
        FakeBluez fake;
        fake.addDial(true);
        BluezLink link(QDBusConnection::sessionBus());
        QSignalSpy online(&link, &BluezLink::online);
        link.start();
        waitState(link, BluezLink::State::Online);
        QVERIFY(fake.notifying());
        QTest::qWait(4300);
        const auto beats = fake.heartbeats();
        QVERIFY2(beats.size() >= 5, qPrintable(QString("only %1 heartbeats").arg(beats.size())));
        QVERIFY(beats.size() <= 6);
        for (int i = 1; i < beats.size(); ++i) {
            const qint64 gap = beats[i].at - beats[i - 1].at;
            QVERIFY2(qAbs(gap - 1000) <= 100, qPrintable(QString("gap %1 ms between heartbeat %2 and %3").arg(gap).arg(i - 1).arg(i)));
        }
        QCOMPARE(beats[0].type, QString("command"));
        QCOMPARE(online.count(), 1);
    }

    void waitsForServicesResolvedBeforeSubscribing() {
        FakeBluez fake;
        fake.addDial(false);
        fake.setConnectDelay(-1);              // Connect() succeeds but the link never comes up by itself
        BluezLink link(QDBusConnection::sessionBus());
        link.start();
        QTRY_VERIFY_WITH_TIMEOUT(fake.count("Connect") >= 1, 2000);
        QCOMPARE(fake.count("StartNotify"), 0);
        QVERIFY(link.state() != BluezLink::State::Online);
        fake.setConnected(true);
        waitState(link, BluezLink::State::Online);
        QCOMPARE(fake.count("StartNotify"), 1);
    }

    void reconnectsAfterConnectedFalseAndResumesHeartbeat() {
        FakeBluez fake;
        fake.addDial(true);
        fake.setConnectDelay(100);
        BluezLink link(QDBusConnection::sessionBus());
        QSignalSpy online(&link, &BluezLink::online);
        link.setBackoff(200, 800);
        link.start();
        waitState(link, BluezLink::State::Online);
        fake.clearRecords();
        fake.setConnected(false);              // host switch / power loss
        QTRY_VERIFY_WITH_TIMEOUT(link.state() != BluezLink::State::Online, 1000);
        QTRY_COMPARE_WITH_TIMEOUT(online.count(), 2, 5000);
        waitState(link, BluezLink::State::Online);
        QVERIFY(fake.count("Connect") >= 1);
        QVERIFY(fake.count("StartNotify") >= 1);
        QTest::qWait(1200);
        QVERIFY(fake.heartbeats().size() >= 2);
    }

    void noHeartbeatsWhileOffline() {
        FakeBluez fake;
        fake.addDial(true);
        fake.setConnectDelay(-1);
        BluezLink link(QDBusConnection::sessionBus());
        link.setBackoff(30000, 30000);
        link.start();
        waitState(link, BluezLink::State::Online);
        fake.setConnected(false);
        QTRY_VERIFY_WITH_TIMEOUT(link.state() != BluezLink::State::Online, 1000);
        fake.clearRecords();
        QTest::qWait(2500);
        QCOMPARE(fake.heartbeats().size(), 0);
    }

    void backoffGrowsAndIsCapped() {
        FakeBluez fake;
        fake.addDial(false);
        fake.setConnectDelay(-1);              // never connects
        BluezLink link(QDBusConnection::sessionBus());
        link.setBackoff(200, 400);
        link.start();
        QTest::qWait(2300);
        // attempts at ~0, 0.2, 0.6, 1.0, 1.4, 1.8 s with the cap at 0.4 s => 5..7, not a tight loop
        const int n = fake.count("Connect");
        QVERIFY2(n >= 4 && n <= 8, qPrintable(QString("%1 connect attempts").arg(n)));
    }

    void doesNotFightABluetoothOffAdapter() {
        FakeBluez fake;
        fake.addDial(false);
        fake.setPowered(false);
        fake.setConnectDelay(-1);
        BluezLink link(QDBusConnection::sessionBus());
        link.start();
        waitState(link, BluezLink::State::BluetoothOff);
        QTest::qWait(800);
        QCOMPARE(fake.count("Connect"), 0);
        fake.setPowered(true);
        QTRY_VERIFY_WITH_TIMEOUT(fake.count("Connect") >= 1, 3000);
    }

    void everyNotifyIsAMessageWithNoBuffering() {
        FakeBluez fake;
        fake.addDial(true);
        BluezLink link(QDBusConnection::sessionBus());
        QList<Protocol::Message> got;
        connect(&link, &BluezLink::received, this, [&](const Protocol::Message &m) { got << m; });
        link.start();
        waitState(link, BluezLink::State::Online);
        fake.notify(QByteArray::fromHex("8b100101"));
        fake.notify(QByteArray::fromHex("8b100100"));
        fake.notify(QByteArray::fromHex("8b100a01"));
        fake.notify(QByteArray::fromHex("0601231001"));
        QTRY_COMPARE_WITH_TIMEOUT(got.size(), 4, 2000);
        using K = Protocol::Message::Kind;
        QCOMPARE(got[0].kind, K::KeyDown);
        QCOMPARE(got[1].kind, K::KeyUp);
        QCOMPARE(got[2].kind, K::Rotate);
        QCOMPARE(got[3].kind, K::Heartbeat);
    }

    void offlineIdTriggersImmediateHeartbeat() {
        FakeBluez fake;
        fake.addDial(true);
        BluezLink link(QDBusConnection::sessionBus());
        link.start();
        waitState(link, BluezLink::State::Online);
        QTest::qWait(300);
        const int before = fake.heartbeats().size();
        fake.notify(QByteArray::fromHex("8b104101"));
        QTRY_VERIFY_WITH_TIMEOUT(fake.heartbeats().size() > before, 500);   // well before the next 1 s tick
    }

    void fallsBackToRequestWhenCommandNotPermitted() {
        FakeBluez fake;
        fake.addDial(true);
        fake.rejectCommandWrites(true);
        BluezLink link(QDBusConnection::sessionBus());
        link.start();
        waitState(link, BluezLink::State::Online);
        QTRY_VERIFY_WITH_TIMEOUT(!fake.heartbeats().isEmpty(), 2000);
        QCOMPARE(fake.heartbeats()[0].type, QString("request"));
        QTest::qWait(1200);
        for (const auto &w : fake.heartbeats()) QCOMPARE(w.type, QString("request"));   // sticky after the first fallback
    }

    void stopUnsubscribesSynchronouslyAndSilencesHeartbeats() {
        FakeBluez fake;
        fake.addDial(true);
        BluezLink link(QDBusConnection::sessionBus());
        link.start();
        waitState(link, BluezLink::State::Online);
        link.stop();                                   // no event-loop turn between this and the check
        QCOMPARE(fake.count("StopNotify"), 1);
        QVERIFY(!fake.notifying());
        fake.clearRecords();
        QTest::qWait(1500);
        QCOMPARE(fake.heartbeats().size(), 0);
        QVERIFY(!link.send(Protocol::heartbeat()));
    }

    void batteryComesFromBattery1() {
        FakeBluez fake;
        fake.addDial(true);
        BluezLink link(QDBusConnection::sessionBus());
        QSignalSpy battery(&link, &BluezLink::batteryChanged);
        link.start();
        waitState(link, BluezLink::State::Online);
        QCOMPARE(link.batteryPercent(), 80);
        fake.setBattery(55);
        QTRY_COMPARE_WITH_TIMEOUT(link.batteryPercent(), 55, 1000);
        QVERIFY(battery.count() >= 1);
    }

    void dialAppearingLaterIsPickedUp() {
        FakeBluez fake;                                // BlueZ is up but nothing is paired yet
        BluezLink link(QDBusConnection::sessionBus());
        link.start();
        waitState(link, BluezLink::State::NotFound);
        fake.addDial(true);
        waitState(link, BluezLink::State::Online);
    }

    void reportsNoBluezWhenServiceIsAbsent() {
        BluezLink link(QDBusConnection::sessionBus(), "org.bluez.absent");
        link.start();
        waitState(link, BluezLink::State::NoBluez);
    }
};

REGISTER_TEST(BluezLinkTests)
#include "tests_bluezlink.moc"
