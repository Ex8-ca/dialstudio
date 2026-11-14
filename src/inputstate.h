// Turns raw dial events into bindable triggers: held-key set, long-press,
// press-rotate fallthrough, modifier-rotate, rotation coalescing and acceleration.
#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QObject>
#include <QTimer>
#include <functional>

class InputState : public QObject {
    Q_OBJECT
public:
    struct Settings {
        int longPressMs = 500;
        int coalesceMs = 40;
        QString acceleration = "mild";   // off | mild | strong
    };

    // Asks whether the active page binds `slot` on `control`, e.g. ("key4", "long_press")
    // or ("knob", "rotate_cw@key4"). This decides how events are interpreted.
    using BindingQuery = std::function<bool(const QString &control, const QString &slot)>;

    explicit InputState(QObject *parent = nullptr);

    void setSettings(const Settings &settings) { m_settings = settings; }
    void setBindingQuery(BindingQuery query) { m_query = std::move(query); }

    void keyDown(int key);
    void keyUp(int key);
    void knobClick();
    void rotate(int direction, bool pressed);   // +1 cw, -1 ccw
    // Connection lost: held keys are forgotten and pending timers dropped.
    void reset();

    QList<int> heldKeys() const { return m_held; }

    static int accelerate(const QString &mode, int count, bool fast);

signals:
    // What `dialstudio events` shows: key1 press, key1 release, key1 long_press,
    // knob click, rotate cw steps=3, press_rotate ccw steps=1.
    void rawEvent(const QString &text);
    // A binding slot to run: control "key4" slot "press", or control "knob" slot "rotate_cw@key4".
    void fire(const QString &control, const QString &slot, int steps);

private:
    struct KeyInfo {
        bool longFired = false;
        bool usedAsModifier = false;
        bool deferred = false;
        QTimer *timer = nullptr;
    };

    bool bound(const QString &control, const QString &slot) const { return m_query && m_query(control, slot); }
    void flushRotation();
    void emitFire(const QString &control, const QString &slot, int steps);
    QString keyControl(int key) const { return QStringLiteral("key%1").arg(key); }

    Settings m_settings;
    BindingQuery m_query;
    QList<int> m_held;               // in press order
    QHash<int, KeyInfo> m_keys;

    QTimer m_coalesce;
    int m_pendingDir = 0;
    bool m_pendingPressed = false;
    int m_pendingCount = 0;
    bool m_pendingFast = false;
    QElapsedTimer m_lastRotate;
};
