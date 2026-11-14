#include "inputstate.h"

#include <cmath>

InputState::InputState(QObject *parent) : QObject(parent) {
    m_coalesce.setSingleShot(true);
    connect(&m_coalesce, &QTimer::timeout, this, &InputState::flushRotation);
}

int InputState::accelerate(const QString &mode, int count, bool fast) {
    // Fast spinning produced events ~14 ms apart; a single slow detent gets no boost.
    if (!fast || count < 2) return count;
    const double factor = mode == "strong" ? 2.5 : mode == "mild" ? 1.5 : 1.0;
    return qMax(count, int(std::lround(count * factor)));
}

void InputState::keyDown(int key) {
    if (m_held.contains(key)) return;
    flushRotation();
    m_held.append(key);
    KeyInfo &info = m_keys[key];
    info = KeyInfo();
    const QString control = keyControl(key);
    emit rawEvent(control + " press");

    // A key that can do something else (long press, or modifier for rotation) must
    // wait for release to know whether it was a plain press.
    bool modifierRotate = false;
    for (const char *slot : {"rotate_cw", "rotate_ccw", "press_rotate_cw", "press_rotate_ccw"})
        modifierRotate = modifierRotate || bound("knob", QStringLiteral("%1@%2").arg(slot, control));
    const bool hasLong = bound(control, "long_press");
    info.deferred = hasLong || modifierRotate;

    if (hasLong) {
        info.timer = new QTimer(this);
        info.timer->setSingleShot(true);
        connect(info.timer, &QTimer::timeout, this, [this, key, control] {
            KeyInfo &i = m_keys[key];
            if (!m_held.contains(key) || i.usedAsModifier) return;
            i.longFired = true;
            emit rawEvent(control + " long_press");
            emitFire(control, "long_press", 1);
        });
        info.timer->start(m_settings.longPressMs);
    } else if (!info.deferred) {
        emitFire(control, "press", 1);
    }
}

void InputState::keyUp(int key) {
    if (!m_held.contains(key)) return;
    flushRotation();
    const QString control = keyControl(key);
    KeyInfo info = m_keys.value(key);
    if (info.timer) { info.timer->stop(); info.timer->deleteLater(); }
    m_held.removeAll(key);
    m_keys.remove(key);
    emit rawEvent(control + " release");
    if (info.deferred && !info.longFired && !info.usedAsModifier)
        emitFire(control, "press", 1);
    emitFire(control, "release", 1);
}

void InputState::knobClick() {
    flushRotation();
    emit rawEvent("knob click");
    emitFire("knob", "click", 1);
}

void InputState::rotate(int direction, bool pressed) {
    const bool changed = m_pendingCount > 0 && (m_pendingDir != direction || m_pendingPressed != pressed);
    if (changed) flushRotation();
    const bool fast = m_lastRotate.isValid() && m_lastRotate.elapsed() < 30;
    m_lastRotate.restart();
    m_pendingDir = direction;
    m_pendingPressed = pressed;
    ++m_pendingCount;
    m_pendingFast = m_pendingFast || fast;
    // The window starts at the first event so latency stays bounded while spinning.
    if (!m_coalesce.isActive()) m_coalesce.start(m_settings.coalesceMs);
}

void InputState::flushRotation() {
    if (m_pendingCount == 0) return;
    m_coalesce.stop();
    const int dir = m_pendingDir;
    const bool pressed = m_pendingPressed;
    const int raw = m_pendingCount;
    const int steps = accelerate(m_settings.acceleration, raw, m_pendingFast);
    m_pendingCount = 0;
    m_pendingFast = false;

    const QString dirName = dir > 0 ? "cw" : "ccw";
    emit rawEvent(QStringLiteral("%1 %2 steps=%3").arg(pressed ? "press_rotate" : "rotate", dirName).arg(raw));

    const QString base = (pressed ? "press_rotate_" : "rotate_") + dirName;
    // Held keys act as modifiers, most recently pressed first.
    for (int i = m_held.size() - 1; i >= 0; --i) {
        const QString slot = QStringLiteral("%1@%2").arg(base, keyControl(m_held[i]));
        if (bound("knob", slot)) {
            m_keys[m_held[i]].usedAsModifier = true;
            emitFire("knob", slot, steps);
            return;
        }
    }
    if (bound("knob", base)) { emitFire("knob", base, steps); return; }
    // Press-rotate without its own binding behaves like plain rotation.
    if (pressed) {
        const QString plain = "rotate_" + dirName;
        for (int i = m_held.size() - 1; i >= 0; --i) {
            const QString slot = QStringLiteral("%1@%2").arg(plain, keyControl(m_held[i]));
            if (bound("knob", slot)) {
                m_keys[m_held[i]].usedAsModifier = true;
                emitFire("knob", slot, steps);
                return;
            }
        }
        if (bound("knob", plain)) emitFire("knob", plain, steps);
    }
}

void InputState::reset() {
    for (auto it = m_keys.begin(); it != m_keys.end(); ++it)
        if (it->timer) { it->timer->stop(); it->timer->deleteLater(); }
    m_keys.clear();
    m_held.clear();
    m_coalesce.stop();
    m_pendingCount = 0;
    m_pendingFast = false;
}

// Only slots with a binding are reported, so listeners never see no-op fires.
void InputState::emitFire(const QString &control, const QString &slot, int steps) {
    if (bound(control, slot)) emit fire(control, slot, steps);
}
