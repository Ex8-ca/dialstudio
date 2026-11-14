// The headless daemon: wires BlueZ link -> input state -> profiles -> actions,
// publishes events and serves the IPC ops used by the CLI (and later the GUI).
#pragma once

#include <QDBusConnection>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QSocketNotifier>

#include "actions.h"
#include "bluezlink.h"
#include "config.h"
#include "inputstate.h"
#include "ipc.h"
#include "profiles.h"

class Daemon : public QObject {
    Q_OBJECT
public:
    struct Options {
        QString configPath;
        QString socketPath;
        bool watchHyprland = true;
        bool probeScope = true;
        bool toasts = true;
        bool dryRunActions = false;   // tests: never spawn real processes
        QString bluezService = "org.bluez";
    };

    Daemon(const QDBusConnection &bus, const Options &options, QObject *parent = nullptr);
    ~Daemon() override;

    bool start(QString *error);
    // Stops heartbeats and unsubscribes first, so the dial falls back to factory mode.
    void halt();
    // Test hooks.
    BluezLink *link() { return &m_link; }
    Config *config() { return &m_config; }
    Profiles *profiles() { return &m_profiles; }
    Actions *actions() { return &m_actions; }
    QJsonObject status() const;
    void handle(const QJsonObject &request, const IpcResponder &respond);
    // Installs SIGINT/SIGTERM handling that calls halt() and emits finished().
    void installSignalHandlers();

signals:
    void finished();

private:
    void onMessage(const Protocol::Message &message);
    void onFire(const QString &control, const QString &slot, int steps);
    void onInternal(const QString &type, const QJsonObject &action);
    void onConfigChanged();
    void applyInputSettings();
    void publish(const QJsonObject &event);
    void toast(const QString &title, const QString &body);
    void refreshActiveWindow();
    QJsonArray loadHyprBinds(bool force);
    QString resolveOmarchy(const QJsonObject &action);
    void checkLinks();
    QString controlSummary(const QJsonObject &action) const;

    QJsonObject opBind(const QJsonObject &request);
    QJsonObject opUnbind(const QJsonObject &request);
    QJsonObject opBindings(const QJsonObject &request) const;
    QJsonObject opSet(const QJsonObject &request, const IpcResponder &respond, bool *deferred);
    QJsonObject opShortcuts();
    QJsonObject opSend(const QJsonObject &request);

    QDBusConnection m_bus;
    Options m_options;
    Config m_config;
    Profiles m_profiles;
    Tools m_tools;
    Actions m_actions;
    InputState m_input;
    BluezLink m_link;
    IpcServer m_server;
    HyprlandWatcher m_hypr;
    QElapsedTimer m_uptime;

    // Device state as last reported by the dial.
    QString m_firmware, m_customCode;
    int m_haptics = -1;       // -1 unknown
    int m_sleep = -1;
    int m_reportPercent = -1;
    bool m_charging = false, m_full = false;
    int m_millivolts = 0;
    QMap<int, QString> m_serialParts;
    QList<IpcResponder> m_hapticsWaiters, m_sleepWaiters;
    QString m_lastError;
    QString m_configError;
    QStringList m_warnings;
    QJsonArray m_bindCache;
    QElapsedTimer m_bindCacheAge;
    bool m_stopping = false;

    int m_signalFd[2] = {-1, -1};
    QSocketNotifier *m_signalNotifier = nullptr;
};
