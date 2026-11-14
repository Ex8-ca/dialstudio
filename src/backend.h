// The GUI's side of the daemon: one asynchronous IPC connection that subscribes to the
// event stream, keeps a model of the device and the current page's bindings, and turns
// edits into validated, atomic saves through the daemon. QML only sees this object.
#pragma once

#include <QHash>
#include <QJsonObject>
#include <QLocalSocket>
#include <QObject>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <functional>

class QProcess;

class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool daemonRunning READ daemonRunning NOTIFY stateChanged)
    // online | connecting | offline | bluetooth-off | not-paired | no-bluez
    Q_PROPERTY(QString connection READ connection NOTIFY stateChanged)
    Q_PROPERTY(int battery READ battery NOTIFY stateChanged)            // -1 unknown
    Q_PROPERTY(bool charging READ charging NOTIFY stateChanged)
    Q_PROPERTY(QString firmware READ firmware NOTIFY stateChanged)
    Q_PROPERTY(int haptics READ haptics NOTIFY stateChanged)            // -1 unknown, 0 off, 1 on
    Q_PROPERTY(QString profile READ profile NOTIFY stateChanged)
    Q_PROPERTY(QString pageName READ pageName NOTIFY stateChanged)
    Q_PROPERTY(QStringList warnings READ warnings NOTIFY stateChanged)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged)      // transient status-bar text
    Q_PROPERTY(bool messageIsError READ messageIsError NOTIFY messageChanged)
    Q_PROPERTY(int revision READ revision NOTIFY bindingsChanged)       // bumps on every change of the bindings
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY historyChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY historyChanged)
    Q_PROPERTY(QVariantList shortcuts READ shortcuts NOTIFY shortcutsChanged)
    Q_PROPERTY(QString shortcutsError READ shortcutsError NOTIFY shortcutsChanged)
    Q_PROPERTY(bool serviceBusy READ serviceBusy NOTIFY serviceChanged)

    Q_PROPERTY(QVariantList profiles READ profiles NOTIFY profilesChanged)
    Q_PROPERTY(QString activeProfile READ activeProfile NOTIFY profilesChanged)
    Q_PROPERTY(bool pinned READ pinned NOTIFY profilesChanged)
    Q_PROPERTY(int sleepSeconds READ sleepSeconds NOTIFY stateChanged)      // -1 unknown
    Q_PROPERTY(QVariantList apps READ apps NOTIFY appsChanged)              // .desktop app picker
    Q_PROPERTY(QVariantList presets READ presets NOTIFY presetsChanged)
    Q_PROPERTY(QVariantList clients READ clients NOTIFY clientsChanged)     // running Hyprland windows

public:
    explicit Backend(const QString &socketPath = {}, QObject *parent = nullptr);
    ~Backend() override;

    bool daemonRunning() const { return m_socket.state() == QLocalSocket::ConnectedState && m_ready; }
    QString connection() const;
    int battery() const { return m_battery; }
    bool charging() const { return m_charging; }
    QString firmware() const { return m_firmware; }
    int haptics() const { return m_haptics; }
    QString profile() const { return m_profile; }
    QString pageName() const { return m_pageName; }
    QStringList warnings() const { return m_warnings; }
    QString message() const { return m_message; }
    bool messageIsError() const { return m_messageError; }
    int revision() const { return m_revision; }
    bool canUndo() const { return !m_undo.isEmpty(); }
    bool canRedo() const { return !m_redo.isEmpty(); }
    QVariantList shortcuts() const { return m_shortcuts; }
    QString shortcutsError() const { return m_shortcutsError; }
    bool serviceBusy() const;

    // --- profiles, pages and presets --------------------------------------------------
    QVariantList profiles() const { return m_profiles; }
    QString activeProfile() const { return m_activeProfile; }
    bool pinned() const { return m_pinned; }
    int sleepSeconds() const { return m_sleep; }
    QVariantList apps() const { return m_apps; }
    QVariantList presets() const { return m_presets; }
    QVariantList clients() const { return m_clients; }
    // The whole profile list is one validated save; the daemon accepts or refuses it whole.
    Q_INVOKABLE void loadProfiles();
    Q_INVOKABLE void saveProfiles(QVariantList profiles);
    Q_INVOKABLE void pinProfile(const QString &name);          // or "auto"
    Q_INVOKABLE void loadApps();
    Q_INVOKABLE void loadClients();
    Q_INVOKABLE void loadPresets();
    Q_INVOKABLE void savePreset(const QString &name, bool overwrite);
    Q_INVOKABLE void applyPreset(const QString &name);
    Q_INVOKABLE void deletePreset(const QString &name);
    Q_INVOKABLE void renamePreset(const QString &from, const QString &to);
    Q_INVOKABLE void setSleep(int seconds);

    // --- bindings model (current profile, page 0 until M2 adds pages) -----------------
    Q_INVOKABLE QVariantMap slotAction(const QString &control, const QString &slot) const;
    Q_INVOKABLE QString slotLabel(const QString &control, const QString &slot) const;
    Q_INVOKABLE QString actionLabel(const QVariantMap &action) const;
    Q_INVOKABLE QVariantMap defaultAction(const QString &type) const;
    // Whether the draft has what the daemon needs, so half-typed edits are not saved.
    Q_INVOKABLE bool draftReady(const QVariantMap &action) const;
    Q_INVOKABLE void setBinding(const QString &control, const QString &slot, const QVariantMap &action);
    Q_INVOKABLE void clearBinding(const QString &control, const QString &slot);
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();

    // --- device and daemon -------------------------------------------------------------
    Q_INVOKABLE void testAction(const QVariantMap &action);
    Q_INVOKABLE void setHaptics(bool on);
    Q_INVOKABLE void loadShortcuts();
    Q_INVOKABLE void startService();
    Q_INVOKABLE void refresh();

    // --- shortcut recording helpers ------------------------------------------------------
    // Qt key + modifiers -> "ctrl+shift+z"; empty while only modifiers are held.
    Q_INVOKABLE QString comboFromKeyEvent(int qtKey, int qtModifiers) const;
    Q_INVOKABLE QStringList modifierChips(int qtModifiers) const;
    Q_INVOKABLE QStringList comboChips(const QString &combo) const;
    Q_INVOKABLE QString normalizeCombo(const QString &text) const;   // "" when it is not a valid combo
    Q_INVOKABLE QString controlTitle(const QString &control) const;
    Q_INVOKABLE QString slotTitle(const QString &control, const QString &slot) const;
    Q_INVOKABLE QStringList slotsOf(const QString &control) const;

signals:
    void stateChanged();
    void messageChanged();
    void bindingsChanged();
    void historyChanged();
    void shortcutsChanged();
    void serviceChanged();
    void profilesChanged();
    void appsChanged();
    void presetsChanged();
    void clientsChanged();
    // A decoded dial event from the daemon's stream: kind is press, release, long_press,
    // click, rotate or press_rotate; direction is cw or ccw for the last two.
    void inputEvent(const QString &control, const QString &kind, const QString &direction, int steps);
    void testFinished(bool ok, const QString &message);
    void editFailed(const QString &message);

private:
    using Callback = std::function<void(const QJsonObject &reply)>;

    void connectNow();
    void onConnected();
    void onDisconnected();
    void onReadyRead();
    void onLine(const QJsonObject &line);
    void send(const QJsonObject &request, Callback callback = {});
    void requestStatus();
    void scheduleStatus();
    void requestBindings();
    void applyStatus(const QJsonObject &status);
    void applyBindings(const QJsonObject &bindings, bool fromServer);
    void commit(const QJsonObject &after, const QJsonObject &before, bool pushUndo, bool isRedo);
    void showMessage(const QString &text, bool error);
    QString linkName() const { return m_link; }

    QLocalSocket m_socket;
    QString m_path;
    QByteArray m_buffer;
    QTimer m_reconnect, m_statusTimer, m_messageTimer, m_modelWatch;
    int m_modelRetries = 0;
    bool m_ready = false;
    int m_nextId = 1;
    QHash<int, Callback> m_pending;

    QString m_link;
    int m_battery = -1;
    bool m_charging = false;
    QString m_firmware;
    int m_haptics = -1;
    QString m_profile, m_pageName;
    QStringList m_warnings;
    QString m_message;
    bool m_messageError = false;
    QJsonObject m_device;

    QJsonObject m_bindings;
    int m_revision = 0;
    int m_pendingSets = 0;
    QList<QJsonObject> m_undo, m_redo;

    QVariantList m_shortcuts;
    QString m_shortcutsError;
    QProcess *m_service = nullptr;

    QVariantList m_profiles, m_apps, m_presets, m_clients;
    QString m_activeProfile;
    bool m_pinned = false;
    int m_sleep = -1;
};
