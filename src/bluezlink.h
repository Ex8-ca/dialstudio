// BlueZ transport over QtDBus: finds the dial by its vendor service UUID, resolves
// the notify/write characteristics by UUID (never by handle), keeps online mode
// alive with a 1 s heartbeat, and reconnects with backoff.
#pragma once

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusServiceWatcher>
#include <QHash>
#include <QMap>
#include <QObject>
#include <QTimer>
#include <QVariantMap>

#include "protocol.h"

using BluezInterfaces = QMap<QString, QVariantMap>;
using BluezObjects = QMap<QDBusObjectPath, BluezInterfaces>;
Q_DECLARE_METATYPE(BluezInterfaces)
Q_DECLARE_METATYPE(BluezObjects)

namespace BluezUuid {
inline const QString Service = QStringLiteral("00001971-0000-1000-8000-00805f9b34fb");
inline const QString Notify = QStringLiteral("0000fd00-0000-1000-8000-00805f9b34fb");
inline const QString Write = QStringLiteral("0000fd01-0000-1000-8000-00805f9b34fb");
}

// The parts of BlueZ's object tree we care about, with the lookups as pure functions
// so `doctor` and the tests can use them without a link.
class BluezModel {
public:
    void clear() { m_objects.clear(); }
    void replaceAll(const BluezObjects &objects);
    void add(const QString &path, const BluezInterfaces &interfaces);
    void remove(const QString &path, const QStringList &interfaces);
    // Returns true if a property of a known object changed.
    bool update(const QString &path, const QString &interface, const QVariantMap &changed, const QStringList &invalidated);

    QVariant property(const QString &path, const QString &interface, const QString &name) const;
    bool has(const QString &path, const QString &interface) const;

    // The device exposing the vendor service; falls back to name / modalias.
    QString findDevice(const QString &address) const;
    QString adapterOf(const QString &devicePath) const;
    QString vendorServicePath(const QString &devicePath) const;
    // Characteristic path with the given UUID under the device's vendor service, or empty.
    QString characteristic(const QString &devicePath, const QString &uuid) const;

private:
    QHash<QString, BluezInterfaces> m_objects;
};

class BluezLink : public QObject {
    Q_OBJECT
public:
    enum class State {
        Stopped,
        NoBluez,        // bluetoothd is not on the bus
        BluetoothOff,   // adapter powered off; we do not fight the user
        NotFound,       // no paired dial known to BlueZ
        Disconnected,   // known, not connected; reconnecting with backoff
        Connecting,
        Resolving,      // connected, waiting for GATT services
        Online,         // notifying + heartbeats running
    };
    Q_ENUM(State)

    explicit BluezLink(const QDBusConnection &bus, const QString &service = "org.bluez", QObject *parent = nullptr);
    ~BluezLink() override;

    void setAddress(const QString &address) { m_address = address.toUpper(); }
    void setBackoff(int firstMs, int maxMs) { m_backoffFirst = firstMs; m_backoffMax = maxMs; }

    void start();
    // Stops heartbeats and calls StopNotify synchronously so the dial reverts to factory mode.
    void stop();

    State state() const { return m_state; }
    static QString stateName(State state);
    QString devicePath() const { return m_devicePath; }
    QString deviceName() const;
    QString deviceAddress() const;
    bool deviceConnected() const;
    int batteryPercent() const;   // -1 when BlueZ has no Battery1 for the device

    // Sends an already-encrypted packet. False when not online.
    bool send(const QByteArray &packet);

    // Adapters, for tests and doctor.
    const BluezModel &model() const { return m_model; }

signals:
    void stateChanged(BluezLink::State state);
    void online();                                   // fired each time online mode is (re)established
    void received(const Protocol::Message &message);
    void batteryChanged(int percent);
    void log(const QString &line);

private slots:
    void onPropertiesChanged(const QDBusMessage &message);
    void onInterfacesAdded(const QDBusMessage &message);
    void onInterfacesRemoved(const QDBusMessage &message);

private:
    void fetchObjects();
    void evaluate();
    void setState(State state);
    void goOffline();
    void startNotify();
    void scheduleConnect();
    void connectDevice();
    void sendHeartbeat();
    void write(const QByteArray &packet, bool command);

    QDBusConnection m_bus;
    QString m_service;
    QString m_address;
    QDBusServiceWatcher m_watcher;
    BluezModel m_model;
    State m_state = State::Stopped;
    bool m_started = false;
    bool m_haveObjects = false;

    QString m_devicePath, m_notifyPath, m_writePath;
    bool m_subscribing = false;
    bool m_online = false;
    bool m_writeAsCommand = true;
    int m_lastBattery = -2;

    QTimer m_heartbeat;
    QTimer m_connectTimer;
    bool m_connectInFlight = false;
    int m_backoffFirst = 2000, m_backoffMax = 30000;
    int m_nextBackoff = 0;
    bool m_firstAttempt = true;
};
