// A fake BlueZ on the (private) session bus: ObjectManager, Device1, Adapter1,
// Battery1 and GattCharacteristic1, recording every write and able to emit notifies.
// It runs in its own thread so the link's blocking StopNotify cannot deadlock it.
#pragma once

#include <QByteArray>
#include <QDBusConnection>
#include <QDBusVirtualObject>
#include <QElapsedTimer>
#include <QMutex>
#include <QThread>
#include <QTimer>
#include <QVariantMap>

#include "bluezlink.h"

class FakeObject;

class FakeBluez {
public:
    struct Write {
        qint64 at;          // ms since the fake started
        QByteArray plain;   // decrypted
        QString type;
    };

    FakeBluez(const QString &serviceName = "org.bluez");
    ~FakeBluez();

    static constexpr const char *Adapter = "/org/bluez/hci0";
    static constexpr const char *Device = "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF";
    static constexpr const char *Service = "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF/service0010";
    static constexpr const char *NotifyChar = "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF/service0010/char0011";
    static constexpr const char *WriteChar = "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF/service0010/char0014";

    // Controls (thread-safe).
    void addDial(bool connected);                 // emits InterfacesAdded for the dial objects
    void setConnected(bool connected);            // flips Connected/ServicesResolved and emits PropertiesChanged
    void setPowered(bool powered);
    void setBattery(int percent);
    void notify(const QByteArray &plain);         // encrypts and emits a Value change on the notify char
    void rejectCommandWrites(bool reject);
    void autoReply(bool on);                      // answer reads/writes of haptics, sleep, firmware
    void setConnectDelay(int ms);

    QList<Write> writes() const;
    QList<Write> heartbeats() const;
    int count(const QString &method) const;       // StartNotify, StopNotify, Connect
    bool notifying() const;
    void clearRecords();

private:
    friend class FakeObject;
    QThread m_thread;
    FakeObject *m_object = nullptr;
};
