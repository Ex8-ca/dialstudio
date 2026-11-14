#include "fakebluez.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QMutexLocker>

#include "protocol.h"
#include "teacodec.h"

class FakeObject : public QDBusVirtualObject {
public:
    FakeObject(const QString &name) : m_name(name) {}

    QString introspect(const QString &) const override { return QString(); }

    void start() {
        qDBusRegisterMetaType<BluezInterfaces>();
        qDBusRegisterMetaType<BluezObjects>();
        m_bus = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "fakebluez-" + m_name);
        m_bus.registerVirtualObject("/", this, QDBusConnection::SubPath);
        m_bus.registerService(m_name);
        m_clock.start();
        m_connectTimer = new QTimer;
        m_connectTimer->setSingleShot(true);
        QObject::connect(m_connectTimer, &QTimer::timeout, [this] { setConnectedLocked(true); });
    }

    void stop() {
        delete m_connectTimer;
        m_bus.unregisterService(m_name);
        m_bus.unregisterObject("/");
        QDBusConnection::disconnectFromBus("fakebluez-" + m_name);
    }

    // --- object model --------------------------------------------------
    BluezInterfaces dialInterfaces(const QString &path) const {
        BluezInterfaces i;
        if (path == FakeBluez::Device) {
            i["org.bluez.Device1"] = {{"Address", "AA:BB:CC:DD:EE:FF"}, {"Name", "Ulanzi Dial"},
                                      {"Adapter", QVariant::fromValue(QDBusObjectPath(FakeBluez::Adapter))},
                                      {"Paired", true}, {"Trusted", true}, {"Connected", m_connected},
                                      {"ServicesResolved", m_connected}, {"Modalias", "usb:vFFF1p0082d0000"}};
            i["org.bluez.Battery1"] = {{"Percentage", QVariant::fromValue(quint8(m_battery))}};
        } else if (path == FakeBluez::Service) {
            i["org.bluez.GattService1"] = {{"UUID", BluezUuid::Service},
                                           {"Device", QVariant::fromValue(QDBusObjectPath(FakeBluez::Device))}};
        } else if (path == FakeBluez::NotifyChar) {
            i["org.bluez.GattCharacteristic1"] = {{"UUID", BluezUuid::Notify},
                                                  {"Service", QVariant::fromValue(QDBusObjectPath(FakeBluez::Service))}};
        } else if (path == FakeBluez::WriteChar) {
            i["org.bluez.GattCharacteristic1"] = {{"UUID", BluezUuid::Write},
                                                  {"Service", QVariant::fromValue(QDBusObjectPath(FakeBluez::Service))}};
        }
        return i;
    }

    BluezObjects objects() const {
        BluezObjects all;
        all[QDBusObjectPath(FakeBluez::Adapter)]["org.bluez.Adapter1"] = {{"Powered", m_powered}};
        // A decoy device that must never be picked.
        all[QDBusObjectPath("/org/bluez/hci0/dev_11_22_33_44_55_66")]["org.bluez.Device1"] = {
            {"Address", "11:22:33:44:55:66"}, {"Name", "Some Keyboard"},
            {"Adapter", QVariant::fromValue(QDBusObjectPath(FakeBluez::Adapter))}, {"Connected", true}};
        if (m_present)
            for (const char *p : {FakeBluez::Device, FakeBluez::Service, FakeBluez::NotifyChar, FakeBluez::WriteChar})
                all[QDBusObjectPath(p)] = dialInterfaces(p);
        return all;
    }

    void emitChanged(const QString &path, const QString &interface, const QVariantMap &changed) {
        QDBusMessage signal = QDBusMessage::createSignal(path, "org.freedesktop.DBus.Properties", "PropertiesChanged");
        signal.setArguments({interface, changed, QStringList()});
        m_bus.send(signal);
    }

    void addDial(bool connected) {
        m_present = true;
        m_connected = connected;
        for (const char *p : {FakeBluez::Device, FakeBluez::Service, FakeBluez::NotifyChar, FakeBluez::WriteChar}) {
            QDBusMessage signal = QDBusMessage::createSignal("/", "org.freedesktop.DBus.ObjectManager", "InterfacesAdded");
            signal.setArguments({QVariant::fromValue(QDBusObjectPath(p)), QVariant::fromValue(dialInterfaces(p))});
            m_bus.send(signal);
        }
    }

    void setConnectedLocked(bool connected) {
        m_connected = connected;
        if (!connected) m_notifying = false;
        emitChanged(FakeBluez::Device, "org.bluez.Device1", {{"Connected", connected}, {"ServicesResolved", connected}});
    }

    void notifyPlain(const QByteArray &plain) {
        QByteArray padded = plain + QByteArray(TeaCodec::PacketSize - plain.size(), '\0');
        emitChanged(FakeBluez::NotifyChar, "org.bluez.GattCharacteristic1", {{"Value", TeaCodec::encrypt(padded)}});
    }

    // --- message handling ------------------------------------------------
    bool handleMessage(const QDBusMessage &message, const QDBusConnection &connection) override {
        if (message.type() != QDBusMessage::MethodCallMessage) return false;
        QMutexLocker lock(&m_mutex);
        const QString interface = message.interface(), member = message.member(), path = message.path();
        auto reply = [&](const QList<QVariant> &args = {}) {
            QDBusMessage r = message.createReply();
            r.setArguments(args);
            connection.send(r);
            return true;
        };
        auto error = [&](const QString &name, const QString &text) {
            connection.send(message.createErrorReply(name, text));
            return true;
        };

        if (interface == "org.freedesktop.DBus.ObjectManager" && member == "GetManagedObjects")
            return reply({QVariant::fromValue(objects())});
        if (interface == "org.bluez.Device1" && member == "Connect") {
            ++m_counts["Connect"];
            if (m_connectDelay >= 0) QMetaObject::invokeMethod(m_connectTimer, [this] { m_connectTimer->start(m_connectDelay); });
            return reply();
        }
        if (interface == "org.bluez.GattCharacteristic1") {
            if (member == "StartNotify") { ++m_counts["StartNotify"]; m_notifying = true; return reply(); }
            if (member == "StopNotify") { ++m_counts["StopNotify"]; m_notifying = false; return reply(); }
            if (member == "WriteValue" && message.arguments().size() >= 2) {
                QVariantMap options;
                message.arguments().at(1).value<QDBusArgument>() >> options;
                const QString type = options.value("type").value<QDBusVariant>().variant().toString().isEmpty()
                                         ? options.value("type").toString()
                                         : options.value("type").value<QDBusVariant>().variant().toString();
                if (m_rejectCommand && type == "command") return error("org.bluez.Error.NotPermitted", "not permitted");
                const QByteArray plain = TeaCodec::decrypt(message.arguments().at(0).toByteArray());
                m_writes.append({m_clock.elapsed(), plain, type});
                if (m_autoReply) answer(plain);
                return reply();
            }
        }
        return error("org.freedesktop.DBus.Error.UnknownMethod", interface + "." + member);
    }

    // Mimics the dial well enough for daemon-level tests.
    void answer(const QByteArray &p) {
        auto b = [&](int i) { return quint8(p[i]); };
        if (b(0) == 0x01 && b(1) == 0x06 && b(2) == 0x40 && b(3) == 0x01) notifyPlain(QByteArray::fromHex("01064011") + char(m_haptics ? 0xff : 0));
        else if (b(0) == 0x01 && b(1) == 0x06 && b(2) == 0x40 && b(3) == 0x04) { m_haptics = b(4) != 0; notifyPlain(QByteArray::fromHex("01064014") + char(m_haptics ? 0xff : 0)); }
        else if (b(0) == 0x01 && b(1) == 0x01 && b(2) == 0x42 && b(3) == 0x01) notifyPlain(QByteArray::fromHex("01014211") + char(m_sleep & 0xff) + char(m_sleep >> 8));
        else if (b(0) == 0x01 && b(1) == 0x01 && b(2) == 0x42 && b(3) == 0x02) { m_sleep = b(4) | (b(5) << 8); notifyPlain(QByteArray::fromHex("01014212") + char(m_sleep & 0xff) + char(m_sleep >> 8)); }
        else if (b(0) == 0x01 && b(1) == 0x04 && b(2) == 0x04 && b(3) == 0x01) notifyPlain(QByteArray::fromHex("0104041140003728280101030826040818000100"));
        else if (b(0) == 0x01 && b(1) == 0x01 && b(2) == 0x0b && b(3) == 0x01) {
            notifyPlain(QByteArray::fromHex("01010b110a004130443236493030335500000000"));
            notifyPlain(QByteArray::fromHex("01010b11070133373131313137000000000000"));
            notifyPlain(QByteArray::fromHex("01010b11070133373131313137000000000000"));   // duplicate fragment
        }
    }

    QString m_name;
    QDBusConnection m_bus{"unset"};
    QElapsedTimer m_clock;
    QTimer *m_connectTimer = nullptr;
    mutable QMutex m_mutex;
    bool m_present = false, m_connected = false, m_powered = true, m_notifying = false;
    bool m_rejectCommand = false, m_autoReply = false;
    bool m_haptics = true;
    int m_sleep = 7200, m_battery = 80, m_connectDelay = 50;
    QList<FakeBluez::Write> m_writes;
    QHash<QString, int> m_counts;
};

FakeBluez::FakeBluez(const QString &serviceName) {
    m_object = new FakeObject(serviceName);
    m_object->moveToThread(&m_thread);
    m_thread.start();
    QMetaObject::invokeMethod(m_object, [this] { m_object->start(); }, Qt::BlockingQueuedConnection);
}

FakeBluez::~FakeBluez() {
    QMetaObject::invokeMethod(m_object, [this] { m_object->stop(); }, Qt::BlockingQueuedConnection);
    m_thread.quit();
    m_thread.wait();
    delete m_object;
}

#define ON_THREAD(code) QMetaObject::invokeMethod(m_object, [=, this] { QMutexLocker l(&m_object->m_mutex); code; }, Qt::BlockingQueuedConnection)

void FakeBluez::addDial(bool connected) { QMetaObject::invokeMethod(m_object, [=, this] { m_object->addDial(connected); }, Qt::BlockingQueuedConnection); }
void FakeBluez::setConnected(bool connected) { QMetaObject::invokeMethod(m_object, [=, this] { m_object->setConnectedLocked(connected); }, Qt::BlockingQueuedConnection); }
void FakeBluez::setPowered(bool powered) {
    QMetaObject::invokeMethod(m_object, [=, this] {
        m_object->m_powered = powered;
        m_object->emitChanged(Adapter, "org.bluez.Adapter1", {{"Powered", powered}});
    }, Qt::BlockingQueuedConnection);
}
void FakeBluez::setBattery(int percent) {
    QMetaObject::invokeMethod(m_object, [=, this] {
        m_object->m_battery = percent;
        m_object->emitChanged(Device, "org.bluez.Battery1", {{"Percentage", QVariant::fromValue(quint8(percent))}});
    }, Qt::BlockingQueuedConnection);
}
void FakeBluez::notify(const QByteArray &plain) { QMetaObject::invokeMethod(m_object, [=, this] { m_object->notifyPlain(plain); }, Qt::BlockingQueuedConnection); }
void FakeBluez::rejectCommandWrites(bool reject) { ON_THREAD(m_object->m_rejectCommand = reject); }
void FakeBluez::autoReply(bool on) { ON_THREAD(m_object->m_autoReply = on); }
void FakeBluez::setConnectDelay(int ms) { ON_THREAD(m_object->m_connectDelay = ms); }

QList<FakeBluez::Write> FakeBluez::writes() const { QMutexLocker l(&m_object->m_mutex); return m_object->m_writes; }
QList<FakeBluez::Write> FakeBluez::heartbeats() const {
    QList<Write> out;
    for (const auto &w : writes())
        if (w.plain.startsWith(QByteArray::fromHex("0601230001"))) out << w;
    return out;
}
int FakeBluez::count(const QString &method) const { QMutexLocker l(&m_object->m_mutex); return m_object->m_counts.value(method); }
bool FakeBluez::notifying() const { QMutexLocker l(&m_object->m_mutex); return m_object->m_notifying; }
void FakeBluez::clearRecords() { QMutexLocker l(&m_object->m_mutex); m_object->m_writes.clear(); m_object->m_counts.clear(); }
