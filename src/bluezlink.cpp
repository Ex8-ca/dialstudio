#include "bluezlink.h"

#include <QDBusArgument>
#include <QDBusError>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusReply>

namespace {

const char *Iface = "org.bluez.GattCharacteristic1";

void registerTypes() {
    static bool done = false;
    if (done) return;
    done = true;
    qDBusRegisterMetaType<BluezInterfaces>();
    qDBusRegisterMetaType<BluezObjects>();
}

// Properties that BlueZ sends as a{sv} arrive as nested QDBusArguments; the ones
// we use are plain strings, bools, object paths and byte arrays.
QVariant unwrap(const QVariant &v) {
    if (v.metaType().id() == qMetaTypeId<QDBusVariant>()) return v.value<QDBusVariant>().variant();
    return v;
}

QVariantMap unwrapMap(const QVariantMap &map) {
    QVariantMap out;
    for (auto it = map.begin(); it != map.end(); ++it)
        out.insert(it.key(), unwrap(it.value()));
    return out;
}

QString pathOf(const QVariant &v) {
    if (v.metaType().id() == qMetaTypeId<QDBusObjectPath>()) return v.value<QDBusObjectPath>().path();
    return v.toString();
}

} // namespace

// ---------------------------------------------------------------------------
// BluezModel

void BluezModel::replaceAll(const BluezObjects &objects) {
    m_objects.clear();
    for (auto it = objects.begin(); it != objects.end(); ++it)
        add(it.key().path(), it.value());
}

void BluezModel::add(const QString &path, const BluezInterfaces &interfaces) {
    BluezInterfaces &slot = m_objects[path];
    for (auto it = interfaces.begin(); it != interfaces.end(); ++it)
        slot[it.key()] = unwrapMap(it.value());
}

void BluezModel::remove(const QString &path, const QStringList &interfaces) {
    auto it = m_objects.find(path);
    if (it == m_objects.end()) return;
    if (interfaces.isEmpty()) { m_objects.erase(it); return; }
    for (const auto &i : interfaces) it->remove(i);
    if (it->isEmpty()) m_objects.erase(it);
}

bool BluezModel::update(const QString &path, const QString &interface, const QVariantMap &changed, const QStringList &invalidated) {
    auto it = m_objects.find(path);
    if (it == m_objects.end() || !it->contains(interface)) return false;
    QVariantMap &props = (*it)[interface];
    for (auto c = changed.begin(); c != changed.end(); ++c) props.insert(c.key(), unwrap(c.value()));
    for (const auto &name : invalidated) props.remove(name);
    return true;
}

QVariant BluezModel::property(const QString &path, const QString &interface, const QString &name) const {
    return m_objects.value(path).value(interface).value(name);
}

bool BluezModel::has(const QString &path, const QString &interface) const {
    return m_objects.value(path).contains(interface);
}

QString BluezModel::findDevice(const QString &address) const {
    QStringList candidates;
    // Primary: the device that owns a GATT service with the vendor UUID.
    for (auto it = m_objects.begin(); it != m_objects.end(); ++it) {
        const QVariantMap service = it->value("org.bluez.GattService1");
        if (service.value("UUID").toString().compare(BluezUuid::Service, Qt::CaseInsensitive) == 0) {
            const QString device = pathOf(service.value("Device"));
            if (has(device, "org.bluez.Device1") && !candidates.contains(device)) candidates << device;
        }
    }
    // Fallback: a device not (yet) showing GATT services, by name or modalias.
    for (auto it = m_objects.begin(); it != m_objects.end(); ++it) {
        const QVariantMap device = it->value("org.bluez.Device1");
        if (device.isEmpty() || candidates.contains(it.key())) continue;
        if (device.value("Name").toString().startsWith("Ulanzi Dial") ||
            device.value("Modalias").toString().startsWith("usb:vFFF1p0082", Qt::CaseInsensitive))
            candidates << it.key();
    }
    QString best;
    for (const auto &path : candidates) {
        if (!address.isEmpty() &&
            property(path, "org.bluez.Device1", "Address").toString().compare(address, Qt::CaseInsensitive) != 0)
            continue;
        if (best.isEmpty() || property(path, "org.bluez.Device1", "Connected").toBool()) best = path;
    }
    return best;
}

QString BluezModel::adapterOf(const QString &devicePath) const {
    return pathOf(property(devicePath, "org.bluez.Device1", "Adapter"));
}

QString BluezModel::vendorServicePath(const QString &devicePath) const {
    for (auto it = m_objects.begin(); it != m_objects.end(); ++it) {
        const QVariantMap service = it->value("org.bluez.GattService1");
        if (!service.isEmpty() && pathOf(service.value("Device")) == devicePath &&
            service.value("UUID").toString().compare(BluezUuid::Service, Qt::CaseInsensitive) == 0)
            return it.key();
    }
    return {};
}

QString BluezModel::characteristic(const QString &devicePath, const QString &uuid) const {
    const QString service = vendorServicePath(devicePath);
    if (service.isEmpty()) return {};
    for (auto it = m_objects.begin(); it != m_objects.end(); ++it) {
        const QVariantMap c = it->value(Iface);
        if (!c.isEmpty() && pathOf(c.value("Service")) == service &&
            c.value("UUID").toString().compare(uuid, Qt::CaseInsensitive) == 0)
            return it.key();
    }
    return {};
}

// ---------------------------------------------------------------------------
// BluezLink

QString BluezLink::stateName(State s) {
    switch (s) {
    case State::Stopped: return "stopped";
    case State::NoBluez: return "no-bluez";
    case State::BluetoothOff: return "bluetooth-off";
    case State::NotFound: return "not-paired";
    case State::Disconnected: return "disconnected";
    case State::Connecting: return "connecting";
    case State::Resolving: return "resolving";
    case State::Online: return "online";
    }
    return "unknown";
}

BluezLink::BluezLink(const QDBusConnection &bus, const QString &service, QObject *parent)
    : QObject(parent), m_bus(bus), m_service(service),
      m_watcher(service, bus, QDBusServiceWatcher::WatchForOwnerChange) {
    registerTypes();
    m_heartbeat.setInterval(1000);
    m_heartbeat.setTimerType(Qt::PreciseTimer);
    connect(&m_heartbeat, &QTimer::timeout, this, &BluezLink::sendHeartbeat);
    m_connectTimer.setSingleShot(true);
    connect(&m_connectTimer, &QTimer::timeout, this, &BluezLink::connectDevice);
    connect(&m_watcher, &QDBusServiceWatcher::serviceRegistered, this, [this] { if (m_started) fetchObjects(); });
    connect(&m_watcher, &QDBusServiceWatcher::serviceUnregistered, this, [this] {
        if (!m_started) return;
        goOffline();
        m_model.clear();
        m_haveObjects = false;
        m_devicePath.clear();
        setState(State::NoBluez);
    });
}

BluezLink::~BluezLink() { stop(); }

QString BluezLink::deviceName() const { return m_model.property(m_devicePath, "org.bluez.Device1", "Name").toString(); }
QString BluezLink::deviceAddress() const { return m_model.property(m_devicePath, "org.bluez.Device1", "Address").toString(); }
bool BluezLink::deviceConnected() const { return m_model.property(m_devicePath, "org.bluez.Device1", "Connected").toBool(); }

int BluezLink::batteryPercent() const {
    const QVariant v = m_model.property(m_devicePath, "org.bluez.Battery1", "Percentage");
    return v.isValid() ? v.toInt() : -1;
}

void BluezLink::start() {
    if (m_started) return;
    m_started = true;
    m_bus.connect(m_service, "", "org.freedesktop.DBus.Properties", "PropertiesChanged", this,
                  SLOT(onPropertiesChanged(QDBusMessage)));
    m_bus.connect(m_service, "/", "org.freedesktop.DBus.ObjectManager", "InterfacesAdded", this,
                  SLOT(onInterfacesAdded(QDBusMessage)));
    m_bus.connect(m_service, "/", "org.freedesktop.DBus.ObjectManager", "InterfacesRemoved", this,
                  SLOT(onInterfacesRemoved(QDBusMessage)));
    setState(State::NoBluez);
    fetchObjects();
}

void BluezLink::stop() {
    if (!m_started) return;
    m_started = false;
    m_heartbeat.stop();
    m_connectTimer.stop();
    if (m_online && !m_notifyPath.isEmpty()) {
        // Synchronous on purpose: the process may exit right after this returns.
        QDBusMessage call = QDBusMessage::createMethodCall(m_service, m_notifyPath, Iface, "StopNotify");
        m_bus.call(call, QDBus::Block, 1500);
    }
    m_online = false;
    m_subscribing = false;
    m_bus.disconnect(m_service, "", "org.freedesktop.DBus.Properties", "PropertiesChanged", this, SLOT(onPropertiesChanged(QDBusMessage)));
    m_bus.disconnect(m_service, "/", "org.freedesktop.DBus.ObjectManager", "InterfacesAdded", this, SLOT(onInterfacesAdded(QDBusMessage)));
    m_bus.disconnect(m_service, "/", "org.freedesktop.DBus.ObjectManager", "InterfacesRemoved", this, SLOT(onInterfacesRemoved(QDBusMessage)));
    setState(State::Stopped);
}

void BluezLink::setState(State s) {
    if (s == m_state) return;
    m_state = s;
    emit stateChanged(s);
}

void BluezLink::fetchObjects() {
    QDBusMessage call = QDBusMessage::createMethodCall(m_service, "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
    auto *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(call), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        QDBusPendingReply<BluezObjects> reply = *w;
        if (!m_started) return;
        if (reply.isError()) {
            emit log("GetManagedObjects failed: " + reply.error().message());
            setState(State::NoBluez);
            return;
        }
        m_model.replaceAll(reply.value());
        m_haveObjects = true;
        evaluate();
    });
}

void BluezLink::onInterfacesAdded(const QDBusMessage &message) {
    if (message.arguments().size() < 2) return;
    BluezInterfaces interfaces;
    message.arguments().at(1).value<QDBusArgument>() >> interfaces;
    m_model.add(message.arguments().at(0).value<QDBusObjectPath>().path(), interfaces);
    evaluate();
}

void BluezLink::onInterfacesRemoved(const QDBusMessage &message) {
    if (message.arguments().size() < 2) return;
    m_model.remove(message.arguments().at(0).value<QDBusObjectPath>().path(), message.arguments().at(1).toStringList());
    evaluate();
}

void BluezLink::onPropertiesChanged(const QDBusMessage &message) {
    const auto args = message.arguments();
    if (args.size() < 2) return;
    const QString interface = args.at(0).toString();
    QVariantMap changed;
    args.at(1).value<QDBusArgument>() >> changed;
    changed = unwrapMap(changed);
    const QString path = message.path();

    // Notifications are the hot path: handle before touching the model.
    if (interface == Iface && changed.contains("Value")) {
        if (path == m_notifyPath) {
            const QByteArray value = changed.value("Value").toByteArray();
            // Every 20-byte notify is its own message (pitfall: never buffer).
            const Protocol::Message parsed = Protocol::parse(value);
            if (parsed.kind == Protocol::Message::Kind::Offline) {
                emit log("offline-mode event seen: online mode lapsed, resending heartbeat");
                sendHeartbeat();
            }
            emit received(parsed);
        }
        return;
    }
    const QStringList invalidated = args.size() > 2 ? args.at(2).toStringList() : QStringList();
    if (!m_model.update(path, interface, changed, invalidated)) return;

    if (interface == "org.bluez.Battery1" && path == m_devicePath) {
        const int pct = batteryPercent();
        if (pct != m_lastBattery) { m_lastBattery = pct; emit batteryChanged(pct); }
        return;
    }
    evaluate();
}

void BluezLink::evaluate() {
    if (!m_started || !m_haveObjects) return;
    const QString device = m_model.findDevice(m_address);
    if (device != m_devicePath) {
        goOffline();
        m_devicePath = device;
        m_firstAttempt = true;
        m_nextBackoff = 0;
    }
    if (m_devicePath.isEmpty()) { setState(State::NotFound); return; }

    const QString adapter = m_model.adapterOf(m_devicePath);
    if (!adapter.isEmpty() && m_model.has(adapter, "org.bluez.Adapter1") &&
        !m_model.property(adapter, "org.bluez.Adapter1", "Powered").toBool()) {
        goOffline();
        m_connectTimer.stop();
        setState(State::BluetoothOff);
        return;
    }

    if (!deviceConnected()) {
        if (m_online || m_subscribing) goOffline();
        if (!m_connectInFlight) setState(State::Disconnected);
        scheduleConnect();
        return;
    }
    m_connectTimer.stop();
    m_firstAttempt = true;
    m_nextBackoff = 0;
    if (m_online || m_subscribing) return;

    if (!m_model.property(m_devicePath, "org.bluez.Device1", "ServicesResolved").toBool()) {
        setState(State::Resolving);
        return;
    }
    m_notifyPath = m_model.characteristic(m_devicePath, BluezUuid::Notify);
    m_writePath = m_model.characteristic(m_devicePath, BluezUuid::Write);
    if (m_notifyPath.isEmpty() || m_writePath.isEmpty()) {
        emit log("vendor characteristics (fd00/fd01) not found on the connected device");
        setState(State::Resolving);
        return;
    }
    startNotify();
}

void BluezLink::goOffline() {
    const bool wasOnline = m_online;
    m_heartbeat.stop();
    m_online = false;
    m_subscribing = false;
    if (wasOnline) emit log("link went offline");
}

void BluezLink::startNotify() {
    m_subscribing = true;
    setState(State::Resolving);
    QDBusMessage call = QDBusMessage::createMethodCall(m_service, m_notifyPath, Iface, "StartNotify");
    auto *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(call), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        QDBusPendingReply<> reply = *w;
        if (!m_started || !m_subscribing) return;
        // Another session of ours may already be notifying after a quick reconnect.
        const bool already = reply.isError() && reply.error().name() == "org.bluez.Error.InProgress";
        if (reply.isError() && !already) {
            m_subscribing = false;
            emit log("StartNotify failed: " + reply.error().message());
            return;
        }
        m_subscribing = false;
        m_online = true;
        m_writeAsCommand = true;
        setState(State::Online);
        sendHeartbeat();               // immediately, then every second
        m_heartbeat.start();
        emit online();
    });
}

void BluezLink::sendHeartbeat() {
    if (m_online) write(Protocol::heartbeat(), true);
}

bool BluezLink::send(const QByteArray &packet) {
    if (!m_online) return false;
    write(packet, m_writeAsCommand);
    return true;
}

void BluezLink::write(const QByteArray &packet, bool command) {
    QDBusMessage call = QDBusMessage::createMethodCall(m_service, m_writePath, Iface, "WriteValue");
    QVariantMap options;
    options.insert("type", command ? "command" : "request");
    call.setArguments({QVariant::fromValue(packet), options});
    auto *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(call), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, packet, command](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        QDBusPendingReply<> reply = *w;
        if (!reply.isError()) return;
        if (command && reply.error().name() == "org.bluez.Error.NotPermitted") {
            // Some stacks refuse write-without-response; retry once with a request.
            m_writeAsCommand = false;
            emit log("WriteValue(command) not permitted, falling back to request");
            write(packet, false);
            return;
        }
        emit log("WriteValue failed: " + reply.error().message());
    });
}

void BluezLink::scheduleConnect() {
    if (m_connectTimer.isActive() || m_connectInFlight || m_state == State::BluetoothOff) return;
    // First attempt right away; after that 2, 4, 8 ... capped at 30 s.
    int delay = 0;
    if (!m_firstAttempt) {
        m_nextBackoff = m_nextBackoff == 0 ? m_backoffFirst : qMin(m_nextBackoff * 2, m_backoffMax);
        delay = m_nextBackoff;
    }
    m_firstAttempt = false;
    m_connectTimer.start(delay);
}

void BluezLink::connectDevice() {
    if (!m_started || m_devicePath.isEmpty() || deviceConnected()) return;
    m_connectInFlight = true;
    setState(State::Connecting);
    QDBusMessage call = QDBusMessage::createMethodCall(m_service, m_devicePath, "org.bluez.Device1", "Connect");
    auto *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(call, 30000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        QDBusPendingReply<> reply = *w;
        m_connectInFlight = false;
        if (reply.isError()) emit log("Connect failed: " + reply.error().message());
        if (m_started) evaluate();
    });
}
