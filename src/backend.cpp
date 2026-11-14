#include "backend.h"

#include "actions.h"
#include "config.h"
#include "ipc.h"
#include "shortcut.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <Qt>

namespace {

const QStringList KeySlotList = {"press", "long_press", "release"};
const QStringList KnobSlotList = {"click", "rotate_cw", "rotate_ccw", "press_rotate_cw", "press_rotate_ccw"};

QString capitalised(const QString &word) {
    if (word.isEmpty()) return word;
    return word.at(0).toUpper() + word.mid(1);
}

// "ctrl+shift+z" -> "Ctrl+Shift+Z"; keysym names such as XF86AudioMute stay as they are.
QString pretty(const QString &combo) {
    QStringList parts;
    for (const QString &p : combo.split('+', Qt::SkipEmptyParts)) {
        if (p == "ctrl" || p == "shift" || p == "alt" || p == "super") parts << capitalised(p);
        else parts << (p.size() == 1 ? p.toUpper() : capitalised(p));
    }
    return parts.join('+');
}

QString keysymForQtKey(int key) {
    switch (key) {
    case Qt::Key_Return: case Qt::Key_Enter: return "Return";
    case Qt::Key_Escape: return "Escape";
    case Qt::Key_Tab: case Qt::Key_Backtab: return "Tab";
    case Qt::Key_Backspace: return "BackSpace";
    case Qt::Key_Delete: return "Delete";
    case Qt::Key_Insert: return "Insert";
    case Qt::Key_Home: return "Home";
    case Qt::Key_End: return "End";
    case Qt::Key_PageUp: return "Prior";
    case Qt::Key_PageDown: return "Next";
    case Qt::Key_Up: return "Up";
    case Qt::Key_Down: return "Down";
    case Qt::Key_Left: return "Left";
    case Qt::Key_Right: return "Right";
    case Qt::Key_Space: return "space";
    case Qt::Key_Print: return "Print";
    case Qt::Key_VolumeUp: return "XF86AudioRaiseVolume";
    case Qt::Key_VolumeDown: return "XF86AudioLowerVolume";
    case Qt::Key_VolumeMute: return "XF86AudioMute";
    case Qt::Key_MediaPlay: case Qt::Key_MediaTogglePlayPause: return "XF86AudioPlay";
    case Qt::Key_MediaNext: return "XF86AudioNext";
    case Qt::Key_MediaPrevious: return "XF86AudioPrev";
    case Qt::Key_MediaStop: return "XF86AudioStop";
    case Qt::Key_MonBrightnessUp: return "XF86MonBrightnessUp";
    case Qt::Key_MonBrightnessDown: return "XF86MonBrightnessDown";
    default: break;
    }
    if (key >= Qt::Key_F1 && key <= Qt::Key_F35) return QStringLiteral("F%1").arg(key - Qt::Key_F1 + 1);
    if (key >= 0x20 && key <= 0x7e) return Shortcut::canonicalKey(QString(QChar(key)));
    return {};
}

bool isModifierKey(int key) {
    return key == Qt::Key_Control || key == Qt::Key_Shift || key == Qt::Key_Alt || key == Qt::Key_Meta ||
           key == Qt::Key_AltGr || key == Qt::Key_Super_L || key == Qt::Key_Super_R || key == Qt::Key_CapsLock;
}

} // namespace

Backend::Backend(const QString &socketPath, QObject *parent)
    : QObject(parent), m_path(socketPath.isEmpty() ? Ipc::socketPath() : socketPath) {
    m_reconnect.setInterval(1000);
    connect(&m_reconnect, &QTimer::timeout, this, &Backend::connectNow);
    m_statusTimer.setSingleShot(true);
    m_statusTimer.setInterval(120);
    connect(&m_statusTimer, &QTimer::timeout, this, &Backend::requestStatus);
    m_messageTimer.setSingleShot(true);
    connect(&m_messageTimer, &QTimer::timeout, this, [this] { m_message.clear(); m_messageError = false; emit messageChanged(); });

    connect(&m_socket, &QLocalSocket::connected, this, &Backend::onConnected);
    connect(&m_socket, &QLocalSocket::disconnected, this, &Backend::onDisconnected);
    connect(&m_socket, &QLocalSocket::readyRead, this, &Backend::onReadyRead);
    // A refused connection is the normal "daemon not running" state, not an error to show.
    connect(&m_socket, &QLocalSocket::errorOccurred, this, [this](QLocalSocket::LocalSocketError) {
        if (m_socket.state() == QLocalSocket::UnconnectedState && !m_reconnect.isActive()) m_reconnect.start();
    });
    // A reply that races an early reconnect is dropped with its callback (onConnected clears
    // m_pending). A profile, preset or app list that stays empty after connecting re-asks a
    // few times, which also covers a daemon that only finishes loading its config later.
    m_modelWatch.setInterval(700);
    connect(&m_modelWatch, &QTimer::timeout, this, [this] {
        if (!daemonRunning()) return;                       // retry when connected again
        if (++m_modelRetries > 30) { m_modelWatch.stop(); return; }
        if (m_profiles.isEmpty()) loadProfiles();
        if (m_presets.isEmpty()) loadPresets();
        if (m_apps.isEmpty()) loadApps();
    });
    connect(&m_socket, &QLocalSocket::connected, this, [this] { m_modelRetries = 0; m_modelWatch.start(); });
    connectNow();
    m_reconnect.start();
}

Backend::~Backend() {
    m_socket.disconnect(this);
    m_socket.abort();
}

// ---------------------------------------------------------------------------
// Connection

void Backend::connectNow() {
    if (m_socket.state() != QLocalSocket::UnconnectedState) return;
    m_socket.connectToServer(m_path);
}

void Backend::onConnected() {
    m_reconnect.stop();
    m_buffer.clear();
    m_pending.clear();
    send({{"op", "subscribe"}}, [this](const QJsonObject &reply) {
        if (!reply.value("ok").toBool()) return;
        m_ready = true;
        emit stateChanged();
    });
    requestStatus();
    requestBindings();
    loadShortcuts();
    loadProfiles();
    loadApps();
    loadPresets();
}

void Backend::onDisconnected() {
    m_ready = false;
    m_pending.clear();
    m_link.clear();
    m_battery = -1;
    m_firmware.clear();
    m_haptics = -1;
    m_warnings.clear();
    emit stateChanged();
    if (!m_reconnect.isActive()) m_reconnect.start();
}

void Backend::onReadyRead() {
    m_buffer += m_socket.readAll();
    int nl;
    while ((nl = m_buffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_buffer.left(nl);
        m_buffer.remove(0, nl + 1);
        const QJsonDocument doc = QJsonDocument::fromJson(line);
        if (doc.isObject()) onLine(doc.object());
    }
}

void Backend::send(const QJsonObject &request, Callback callback) {
    if (m_socket.state() != QLocalSocket::ConnectedState) {
        if (callback) callback({{"ok", false}, {"error", tr("The Dial Studio service is not running.")}});
        return;
    }
    QJsonObject withId = request;
    if (callback) {
        const int id = m_nextId++;
        withId.insert("id", id);
        m_pending.insert(id, std::move(callback));
    }
    m_socket.write(Ipc::encode(withId));
}

void Backend::onLine(const QJsonObject &line) {
    if (line.contains("id")) {
        const Callback callback = m_pending.take(line.value("id").toInt());
        if (callback) callback(line);
        return;
    }
    const QString event = line.value("event").toString();
    if (event == "input") {
        const QString control = line.value("control").toString();
        const QString kind = line.value("kind").toString();
        if (!control.isEmpty() && !kind.isEmpty())
            emit inputEvent(control, kind, line.value("direction").toString(), line.value("steps").toInt(1));
        return;
    }
    if (event == "battery") {
        m_battery = line.value("percent").toInt(m_battery);
        if (line.contains("charging")) m_charging = line.value("charging").toBool();
        emit stateChanged();
    } else if (event == "error") {
        showMessage(line.value("action").toString() + ": " + line.value("message").toString(), true);
    } else if (event == "config_error") {
        showMessage(tr("The config file is broken: %1").arg(line.value("message").toString()), true);
    } else if (event == "config") {
        // Someone else (the CLI, an editor) changed the file; our own saves are answered directly.
        if (m_pendingSets == 0) requestBindings();
    } else if (event == "shortcuts") {
        loadShortcuts();
    } else if (event == "config") {
        loadProfiles();                  // a profile edit from another client or the CLI
    }
    if (event == "link" || event == "device" || event == "profile" || event == "config" || event == "shortcuts")
        scheduleStatus();
}

void Backend::scheduleStatus() {
    if (!m_statusTimer.isActive()) m_statusTimer.start();
}

void Backend::requestStatus() {
    send({{"op", "status"}}, [this](const QJsonObject &reply) {
        if (reply.value("ok").toBool()) applyStatus(reply);
    });
}

void Backend::applyStatus(const QJsonObject &s) {
    m_link = s.value("link").toString();
    const QJsonObject battery = s.value("battery").toObject();
    m_battery = battery.value("percent").isNull() ? -1 : battery.value("percent").toInt(-1);
    m_charging = battery.value("charging").toBool();
    m_firmware = s.value("device").toObject().value("firmware").toString();
    m_haptics = s.value("haptics").isNull() ? -1 : (s.value("haptics").toBool() ? 1 : 0);
    const QJsonValue sleep = s.value("sleep_seconds");
    m_sleep = sleep.isNull() ? -1 : sleep.toInt(-1);
    m_warnings.clear();
    for (const auto &w : s.value("warnings").toArray()) m_warnings << w.toString();
    const QString configError = s.value("config").toObject().value("error").toString();
    if (!configError.isEmpty()) m_warnings << tr("Config error: %1").arg(configError);
    const QJsonObject profile = s.value("profile").toObject();
    if (m_profile.isEmpty()) m_profile = profile.value("active").toString();
    m_pageName = profile.value("page_name").toString();
    emit stateChanged();
}

QString Backend::connection() const {
    if (!daemonRunning()) return "offline";
    if (m_link == "online") return "online";
    if (m_link == "bluetooth-off") return "bluetooth-off";
    if (m_link == "not-paired") return "not-paired";
    if (m_link == "no-bluez") return "no-bluez";
    if (m_link == "stopped" || m_link.isEmpty()) return "offline";
    return "connecting";   // disconnected (retrying), connecting, resolving
}

// ---------------------------------------------------------------------------
// Bindings model

void Backend::requestBindings() {
    send({{"op", "bindings_get"}, {"page", 0}}, [this](const QJsonObject &reply) {
        if (!reply.value("ok").toBool()) return;
        m_profile = reply.value("profile").toString();
        applyBindings(reply.value("bindings").toObject(), true);
        m_device = reply.value("device").toObject();
        emit stateChanged();
    });
}

void Backend::applyBindings(const QJsonObject &bindings, bool) {
    if (bindings == m_bindings) return;
    m_bindings = bindings;
    ++m_revision;
    emit bindingsChanged();
}

QVariantMap Backend::slotAction(const QString &control, const QString &slot) const {
    return m_bindings.value(control).toObject().value(slot).toObject().toVariantMap();
}

QString Backend::slotLabel(const QString &control, const QString &slot) const {
    const QVariantMap action = slotAction(control, slot);
    return action.isEmpty() ? QString() : actionLabel(action);
}

QString Backend::actionLabel(const QVariantMap &a) const {
    const QString type = a.value("type").toString();
    const QString action = a.value("action").toString();
    if (type == "keys") {
        if (a.contains("omarchy")) return a.value("omarchy").toString();
        return pretty(a.value("combo").toString());
    }
    if (type == "omarchy_bind") return a.value("description").toString();
    if (type == "volume") return action == "up" ? tr("Volume +") : action == "down" ? tr("Volume −") : tr("Mute");
    if (type == "mic") return tr("Mic mute");
    if (type == "brightness") return action == "up" ? tr("Brightness +") : tr("Brightness −");
    if (type == "media") {
        if (action == "play-pause") return tr("Play/Pause");
        if (action == "next") return tr("Next");
        if (action == "previous") return tr("Previous");
        return tr("Stop");
    }
    if (type == "exec") {
        const QString app = a.value("app").toString();
        if (!app.isEmpty()) return app;
        return QProcess::splitCommand(a.value("command").toString()).value(0);
    }
    if (type == "hypr") {
        if (!a.value("lua").toString().isEmpty()) return tr("Hyprland");
        return a.value("dispatch").toString() + " " + a.value("arg").toString();
    }
    if (type == "omarchy") return a.value("action").toString() == "run" ? a.value("command").toString() : tr("Omarchy %1").arg(action);
    if (type == "shell") return tr("Shell");
    if (type == "text") return tr("Type text");
    if (type == "url") return tr("Open link");
    if (type == "noop") return tr("Nothing");
    if (type == "page") return tr("Page %1").arg(action);
    if (type == "profile") return tr("Profile");
    if (type == "haptics") return tr("Haptics");
    return type;
}

QVariantMap Backend::defaultAction(const QString &type) const {
    QJsonObject a{{"type", type}};
    if (type == "keys") a.insert("combo", "");
    else if (type == "volume") { a.insert("action", "up"); a.insert("step", 2); }
    else if (type == "media") a.insert("action", "play-pause");
    else if (type == "brightness") a.insert("action", "up");
    else if (type == "exec") a.insert("command", "");
    else if (type == "hypr") { a.insert("dispatch", "workspace"); a.insert("arg", "+1"); }
    else if (type == "omarchy_bind") a.insert("description", "");
    else if (type == "omarchy") a.insert("action", "menu");
    else if (type == "shell") a.insert("command", "");
    return a.toVariantMap();
}

bool Backend::draftReady(const QVariantMap &action) const {
    QString error;
    return Actions::validate(QJsonObject::fromVariantMap(action), &error);
}

void Backend::setBinding(const QString &control, const QString &slot, const QVariantMap &action) {
    QString error;
    const QJsonObject a = QJsonObject::fromVariantMap(action);
    if (!Config::validSlot(control, slot, &error) || !Actions::validate(a, &error)) {
        showMessage(error, true);
        emit editFailed(error);
        return;
    }
    const QJsonObject before = m_bindings;
    QJsonObject slotMap = m_bindings.value(control).toObject();
    if (slotMap.value(slot).toObject() == a) return;
    slotMap.insert(slot, a);
    QJsonObject after = m_bindings;
    after.insert(control, slotMap);
    commit(after, before, true, false);
}

void Backend::clearBinding(const QString &control, const QString &slot) {
    QJsonObject slotMap = m_bindings.value(control).toObject();
    if (!slotMap.contains(slot)) return;
    const QJsonObject before = m_bindings;
    slotMap.remove(slot);
    QJsonObject after = m_bindings;
    if (slotMap.isEmpty()) after.remove(control);
    else after.insert(control, slotMap);
    commit(after, before, true, false);
}

// Edits show at once and are saved through the daemon, which validates and writes the file
// atomically and hot reloads itself. A refusal rolls the model back and says why.
void Backend::commit(const QJsonObject &after, const QJsonObject &before, bool pushUndo, bool isRedo) {
    if (!daemonRunning()) {
        showMessage(tr("The service is not running, so nothing was saved."), true);
        emit editFailed(tr("The service is not running."));
        return;
    }
    applyBindings(after, false);
    if (pushUndo) {
        m_undo.append(before);
        if (m_undo.size() > 100) m_undo.removeFirst();
        if (!isRedo) m_redo.clear();
    }
    emit historyChanged();
    ++m_pendingSets;
    send({{"op", "bindings_set"}, {"page", 0}, {"bindings", after}}, [this, before, pushUndo](const QJsonObject &reply) {
        --m_pendingSets;
        if (reply.value("ok").toBool()) {
            // The daemon's copy is the truth (it may have normalised something).
            applyBindings(reply.value("bindings").toObject(), true);
            return;
        }
        const QString error = reply.value("error").toString();
        if (pushUndo && !m_undo.isEmpty()) m_undo.removeLast();
        applyBindings(before, false);
        emit historyChanged();
        showMessage(error, true);
        emit editFailed(error);
        requestBindings();
    });
}

void Backend::undo() {
    if (m_undo.isEmpty()) return;
    const QJsonObject current = m_bindings;
    const QJsonObject previous = m_undo.takeLast();
    m_redo.append(current);
    commit(previous, current, false, false);
}

void Backend::redo() {
    if (m_redo.isEmpty()) return;
    const QJsonObject current = m_bindings;
    const QJsonObject next = m_redo.takeLast();
    m_undo.append(current);
    commit(next, current, false, true);
}

// ---------------------------------------------------------------------------
// Device and daemon

void Backend::testAction(const QVariantMap &action) {
    send({{"op", "test"}, {"action", QJsonObject::fromVariantMap(action)}}, [this](const QJsonObject &reply) {
        const bool ok = reply.value("ok").toBool();
        QString text;
        if (ok) {
            QStringList lines;
            for (const auto &argv : reply.value("argv").toArray()) {
                QStringList parts;
                for (const auto &p : argv.toArray()) parts << p.toString();
                lines << parts.join(' ');
            }
            text = lines.isEmpty() ? tr("Done.") : tr("Ran: %1").arg(lines.join("; "));
        } else {
            text = reply.value("error").toString();
        }
        showMessage(text, !ok);
        emit testFinished(ok, text);
    });
}

void Backend::setHaptics(bool on) {
    send({{"op", "set"}, {"what", "haptics"}, {"value", on}}, [this](const QJsonObject &reply) {
        if (!reply.value("ok").toBool()) showMessage(reply.value("error").toString(), true);
        requestStatus();
    });
}

void Backend::setSleep(int seconds) {
    send({{"op", "set"}, {"what", "sleep"}, {"value", seconds}}, [this](const QJsonObject &reply) {
        if (!reply.value("ok").toBool()) showMessage(reply.value("error").toString(), true);
        requestStatus();
    });
}

void Backend::loadProfiles() {
    send({{"op", "profiles_get"}}, [this](const QJsonObject &reply) {
        if (!reply.value("ok").toBool()) return;
        m_profiles.clear();
        for (const auto &p : reply.value("profiles").toArray()) m_profiles << p.toObject().toVariantMap();
        m_activeProfile = reply.value("active").toString();
        m_pinned = reply.value("pinned").toBool();
        emit profilesChanged();
    });
}

void Backend::saveProfiles(QVariantList list) {
    QJsonArray array;
    for (const QVariant &p : list) array.append(QJsonObject::fromVariantMap(p.toMap()));
    send({{"op", "profiles_set"}, {"profiles", array}}, [this, list](const QJsonObject &reply) {
        if (reply.value("ok").toBool()) {
            emit profilesChanged();       // the daemon's copy is live
            showMessage(tr("Saved."), false);
            loadProfiles();
            requestBindings();
            return;
        }
        const QString error = reply.value("error").toString();
        showMessage(error, true);
        emit editFailed(error);
        emit profilesChanged();           // QML restores its draft when loadProfiles() replaces it
    });
}

void Backend::pinProfile(const QString &name) {
    send({{"op", "profile"}, {"name", name}}, [this, name](const QJsonObject &reply) {
        if (!reply.value("ok").toBool()) { showMessage(reply.value("error").toString(), true); return; }
        if (name == "auto") showMessage(tr("Profiles follow the focused window again."), false);
        showMessage(tr("Profile: %1").arg(reply.value("profile").toString()), false);
        loadProfiles();
    });
}

void Backend::loadApps() {
    send({{"op", "apps"}}, [this](const QJsonObject &reply) {
        m_apps.clear();
        if (reply.value("ok").toBool())
            for (const auto &v : reply.value("apps").toArray()) m_apps << v.toObject().toVariantMap();
        else
            showMessage(reply.value("error").toString(), true);
        emit appsChanged();
    });
}

void Backend::loadClients() {
    send({{"op", "clients"}}, [this](const QJsonObject &reply) {
        m_clients.clear();
        for (const auto &v : reply.value("clients").toArray()) m_clients << v.toObject().toVariantMap();
        if (!reply.value("ok").toBool()) showMessage(reply.value("error").toString(), true);
        emit clientsChanged();
    });
}

void Backend::loadPresets() {
    send({{"op", "presets"}, {"verb", "list"}}, [this](const QJsonObject &reply) {
        m_presets.clear();
        if (reply.value("ok").toBool())
            for (const auto &v : reply.value("presets").toArray()) m_presets << v.toObject().toVariantMap();
        emit presetsChanged();
    });
}

void Backend::savePreset(const QString &name, bool overwrite) {
    send({{"op", "presets"}, {"verb", "save"}, {"name", name}, {"overwrite", overwrite}},
         [this](const QJsonObject &reply) {
             if (!reply.value("ok").toBool()) { showMessage(reply.value("error").toString(), true); return; }
             showMessage(tr("Preset saved."), false);
             loadPresets();
         });
}

void Backend::applyPreset(const QString &name) {
    send({{"op", "presets"}, {"verb", "apply"}, {"name", name}}, [this](const QJsonObject &reply) {
        if (!reply.value("ok").toBool()) { showMessage(reply.value("error").toString(), true); return; }
        showMessage(tr("Preset applied."), false);
        loadProfiles();
        requestBindings();
        requestStatus();
    });
}

void Backend::deletePreset(const QString &name) {
    send({{"op", "presets"}, {"verb", "delete"}, {"name", name}}, [this](const QJsonObject &reply) {
        if (!reply.value("ok").toBool()) { showMessage(reply.value("error").toString(), true); return; }
        loadPresets();
    });
}

void Backend::renamePreset(const QString &from, const QString &to) {
    send({{"op", "presets"}, {"verb", "rename"}, {"name", from}, {"to", to}}, [this](const QJsonObject &reply) {
        if (!reply.value("ok").toBool()) { showMessage(reply.value("error").toString(), true); return; }
        loadPresets();
    });
}

void Backend::loadShortcuts() {
    send({{"op", "shortcuts"}}, [this](const QJsonObject &reply) {
        m_shortcuts.clear();
        if (reply.value("ok").toBool()) {
            m_shortcutsError.clear();
            for (const auto &v : reply.value("shortcuts").toArray()) m_shortcuts << v.toObject().toVariantMap();
        } else {
            m_shortcutsError = reply.value("error").toString();
        }
        emit shortcutsChanged();
    });
}

bool Backend::serviceBusy() const { return m_service && m_service->state() != QProcess::NotRunning; }

void Backend::startService() {
    if (serviceBusy()) return;
    if (!m_service) {
        m_service = new QProcess(this);
        connect(m_service, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
            emit serviceChanged();
            if (code == 0) {
                showMessage(tr("Service started."), false);
                connectNow();
            } else {
                showMessage(tr("Could not start the service: %1").arg(QString::fromUtf8(m_service->readAllStandardError()).trimmed()), true);
            }
        });
        connect(m_service, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
            if (e == QProcess::FailedToStart) {
                emit serviceChanged();
                showMessage(tr("systemctl could not be started."), true);
            }
        });
    }
    // The service unit is only installed by the package; says so rather than failing quietly.
    m_service->start("systemctl", {"--user", "enable", "--now", "dialstudio"});
    emit serviceChanged();
}

void Backend::refresh() {
    requestStatus();
    requestBindings();
    loadShortcuts();
    loadProfiles();
    loadApps();
    loadPresets();
}

void Backend::showMessage(const QString &text, bool error) {
    m_message = text;
    m_messageError = error;
    emit messageChanged();
    m_messageTimer.start(error ? 9000 : 4000);
}

// ---------------------------------------------------------------------------
// Shortcut recording

QStringList Backend::modifierChips(int modifiers) const {
    QStringList chips;
    if (modifiers & Qt::ControlModifier) chips << "Ctrl";
    if (modifiers & Qt::ShiftModifier) chips << "Shift";
    if (modifiers & Qt::AltModifier) chips << "Alt";
    if (modifiers & Qt::MetaModifier) chips << "Super";
    return chips;
}

QString Backend::comboFromKeyEvent(int key, int modifiers) const {
    if (isModifierKey(key)) return {};
    const QString keysym = keysymForQtKey(key);
    if (keysym.isEmpty()) return {};
    QStringList parts;
    if (modifiers & Qt::ControlModifier) parts << "ctrl";
    if (modifiers & Qt::ShiftModifier) parts << "shift";
    if (modifiers & Qt::AltModifier) parts << "alt";
    if (modifiers & Qt::MetaModifier) parts << "super";
    parts << keysym;
    return normalizeCombo(parts.join('+'));
}

QStringList Backend::comboChips(const QString &combo) const {
    QStringList chips;
    for (const QString &p : combo.split('+', Qt::SkipEmptyParts)) chips << (p == "ctrl" || p == "shift" || p == "alt" || p == "super"
                                                                                  ? capitalised(p) : (p.size() == 1 ? p.toUpper() : capitalised(p)));
    return chips;
}

QString Backend::normalizeCombo(const QString &text) const {
    Combo combo;
    QString error;
    return Shortcut::parse(text, &combo, &error) ? combo.toString() : QString();
}

QString Backend::controlTitle(const QString &control) const {
    if (control == "knob") return tr("Knob");
    if (control.startsWith("key")) return tr("Key %1").arg(control.mid(3));
    return control;
}

QString Backend::slotTitle(const QString &, const QString &slot) const {
    if (slot == "press") return tr("Press");
    if (slot == "long_press") return tr("Long press");
    if (slot == "release") return tr("Release");
    if (slot == "click") return tr("Click");
    if (slot == "rotate_cw") return tr("Turn clockwise");
    if (slot == "rotate_ccw") return tr("Turn counter-clockwise");
    if (slot == "press_rotate_cw") return tr("Press and turn clockwise");
    if (slot == "press_rotate_ccw") return tr("Press and turn counter-clockwise");
    return slot;
}

QStringList Backend::slotsOf(const QString &control) const {
    return control == "knob" ? KnobSlotList : KeySlotList;
}
