#include "daemon.h"
#include "version.h"

#include "shortcut.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <csignal>
#include <sys/socket.h>
#include <unistd.h>

namespace {

int g_signalWriteFd = -1;
void signalHandler(int) {
    const char byte = 1;
    // Only async-signal-safe calls here; the notifier does the real work.
    if (g_signalWriteFd >= 0) (void)!::write(g_signalWriteFd, &byte, 1);
}

QJsonObject ok(const QJsonObject &extra = {}) {
    QJsonObject o = extra;
    o.insert("ok", true);
    return o;
}
QJsonObject err(const QString &message) { return {{"ok", false}, {"error", message}}; }

} // namespace

Daemon::Daemon(const QDBusConnection &bus, const Options &options, QObject *parent)
    : QObject(parent), m_bus(bus), m_options(options), m_config(options.configPath), m_profiles(&m_config),
      m_tools(Tools::detect(options.probeScope)), m_actions(m_tools), m_link(bus, options.bluezService) {
    m_actions.setDryRun(options.dryRunActions);
    m_actions.setShortcutResolver([this](const QJsonObject &a) { return resolveOmarchy(a); });
    m_input.setBindingQuery([this](const QString &control, const QString &slot) {
        return m_profiles.hasBinding(control, slot);
    });

    connect(&m_link, &BluezLink::received, this, &Daemon::onMessage);
    connect(&m_link, &BluezLink::stateChanged, this, [this](BluezLink::State s) {
        if (s != BluezLink::State::Online) m_input.reset();   // held keys are forgotten on disconnect
        publish({{"event", "link"}, {"state", BluezLink::stateName(s)}});
    });
    connect(&m_link, &BluezLink::online, this, [this] {
        m_input.reset();
        m_serialParts.clear();
        // Query what status shows; configured settings are re-applied when the replies arrive.
        m_link.send(Protocol::readFirmware());
        m_link.send(Protocol::readHaptics());
        m_link.send(Protocol::readSleep());
        m_link.send(Protocol::readSerial());
    });
    connect(&m_link, &BluezLink::batteryChanged, this, [this](int p) { publish({{"event", "battery"}, {"percent", p}}); });
    connect(&m_link, &BluezLink::log, this, [this](const QString &line) {
        qWarning().noquote() << "bluez:" << line;
        m_lastError = line;
    });

    connect(&m_input, &InputState::rawEvent, this, [this](const QString &text) {
        publish({{"event", "input"}, {"text", text}});
    });
    connect(&m_input, &InputState::fire, this, &Daemon::onFire);
    connect(&m_actions, &Actions::failed, this, [this](const QString &what, const QString &message) {
        m_lastError = what + ": " + message;
        qWarning().noquote() << "action failed:" << what << message;
        publish({{"event", "error"}, {"action", what}, {"message", message}});
    });
    connect(&m_actions, &Actions::internalAction, this, &Daemon::onInternal);

    connect(&m_config, &Config::changed, this, &Daemon::onConfigChanged);
    connect(&m_config, &Config::loadFailed, this, [this](const QString &message) {
        m_configError = message;
        qWarning().noquote() << "config reload failed, keeping the previous config:" << message;
        publish({{"event", "config_error"}, {"message", message}});
    });
    connect(&m_profiles, &Profiles::changed, this, [this] {
        publish({{"event", "profile"}, {"profile", m_profiles.activeName()}, {"page", m_profiles.page()},
                 {"page_name", m_profiles.pageName()}});
    });
    connect(&m_hypr, &HyprlandWatcher::windowChanged, &m_profiles, &Profiles::setActiveWindow);
}

Daemon::~Daemon() {
    halt();
    for (int fd : m_signalFd)
        if (fd >= 0) ::close(fd);
}

bool Daemon::start(QString *error) {
    m_uptime.start();
    QString problem;
    if (!m_config.load(&problem)) {
        // A broken file must never be overwritten: refuse to start and say why.
        if (error) *error = problem;
        return false;
    }
    m_config.setWatching(true);
    applyInputSettings();
    if (!m_server.listen(m_options.socketPath, error)) return false;
    m_server.setHandler([this](const QJsonObject &request, const IpcResponder &respond) { handle(request, respond); });

    m_link.setAddress(m_config.device().value("address").toString());
    m_link.start();
    if (m_options.watchHyprland) {
        m_hypr.start();
        refreshActiveWindow();
    }
    return true;
}

void Daemon::halt() {
    if (m_stopping) return;
    m_stopping = true;
    m_link.stop();
    QLocalServer::removeServer(m_options.socketPath);
}

void Daemon::installSignalHandlers() {
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, m_signalFd) != 0) return;
    g_signalWriteFd = m_signalFd[0];
    m_signalNotifier = new QSocketNotifier(m_signalFd[1], QSocketNotifier::Read, this);
    connect(m_signalNotifier, &QSocketNotifier::activated, this, [this] {
        char byte;
        (void)!::read(m_signalFd[1], &byte, 1);
        halt();
        emit finished();
    });
    struct sigaction action {};
    action.sa_handler = signalHandler;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
}

void Daemon::publish(const QJsonObject &event) { m_server.broadcast(event); }

void Daemon::toast(const QString &title, const QString &body) {
    if (!m_options.toasts || !m_config.osdEnabled() || !m_tools.has("notify-send")) return;
    QProcess::startDetached("notify-send", {"-a", "dialstudio", "-t", "1500", title, body});
}

void Daemon::refreshActiveWindow() {
    if (!m_tools.has("hyprctl")) return;
    auto *process = new QProcess(this);
    connect(process, &QProcess::finished, this, [this, process] {
        const QJsonObject w = QJsonDocument::fromJson(process->readAllStandardOutput()).object();
        if (!w.isEmpty()) m_profiles.setActiveWindow(w.value("class").toString(), w.value("title").toString());
        process->deleteLater();
    });
    connect(process, &QProcess::errorOccurred, process, &QObject::deleteLater);
    process->start("hyprctl", {"-j", "activewindow"});
}

void Daemon::applyInputSettings() {
    const QJsonObject d = m_config.device();
    InputState::Settings s;
    s.longPressMs = d.value("long_press_ms").toInt(500);
    s.coalesceMs = d.value("rotate_coalesce_ms").toInt(40);
    s.acceleration = d.value("acceleration").toString("mild");
    m_input.setSettings(s);
}

void Daemon::onConfigChanged() {
    m_configError.clear();
    applyInputSettings();
    m_bindCacheAge.invalidate();
    checkLinks();
    const QJsonObject d = m_config.device();
    if (d.contains("haptics") && m_haptics >= 0 && d.value("haptics").toBool() != bool(m_haptics))
        m_link.send(Protocol::setHaptics(d.value("haptics").toBool()));
    if (d.contains("sleep_seconds") && m_sleep >= 0 && d.value("sleep_seconds").toInt() != m_sleep)
        m_link.send(Protocol::setSleep(d.value("sleep_seconds").toInt()));
    publish({{"event", "config"}});
}

void Daemon::onMessage(const Protocol::Message &m) {
    using K = Protocol::Message::Kind;
    switch (m.kind) {
    case K::KeyDown: m_input.keyDown(m.key); break;
    case K::KeyUp: m_input.keyUp(m.key); break;
    case K::KnobClick: m_input.knobClick(); break;
    case K::Rotate: m_input.rotate(m.direction, m.pressed); break;
    case K::Offline:
        qWarning() << "offline-mode id" << m.key << "seen; heartbeat resent";
        break;
    case K::Firmware: m_firmware = m.text; m_customCode = m.customCode; break;
    case K::SerialFragment: m_serialParts.insert(m.index, m.text); break;   // deduped by index
    case K::Battery:
        m_reportPercent = m.percent;
        m_charging = m.charging;
        m_full = m.full;
        m_millivolts = m.millivolts;
        publish({{"event", "battery"}, {"percent", m.percent}, {"charging", m.charging}});
        break;
    case K::Haptics: {
        m_haptics = m.on ? 1 : 0;
        const QJsonObject d = m_config.device();
        // A read reply (not an ack) that disagrees with the config is drift to correct.
        if (!m.writeAck && d.contains("haptics") && d.value("haptics").toBool() != m.on)
            m_link.send(Protocol::setHaptics(d.value("haptics").toBool()));
        else {
            for (const auto &respond : std::as_const(m_hapticsWaiters)) respond(ok({{"haptics", m.on}}));
            m_hapticsWaiters.clear();
        }
        publish({{"event", "device"}, {"haptics", m.on}});
        break;
    }
    case K::Sleep: {
        m_sleep = m.seconds;
        const QJsonObject d = m_config.device();
        if (!m.writeAck && d.contains("sleep_seconds") && d.value("sleep_seconds").toInt() != m.seconds)
            m_link.send(Protocol::setSleep(d.value("sleep_seconds").toInt()));
        else {
            for (const auto &respond : std::as_const(m_sleepWaiters)) respond(ok({{"sleep_seconds", m.seconds}}));
            m_sleepWaiters.clear();
        }
        publish({{"event", "device"}, {"sleep_seconds", m.seconds}});
        break;
    }
    default: break;
    }
}

void Daemon::onFire(const QString &control, const QString &slot, int steps) {
    const QJsonObject action = m_profiles.lookup(control, slot);
    if (action.isEmpty()) return;
    m_actions.run(action, steps);
}

void Daemon::onInternal(const QString &type, const QJsonObject &a) {
    const QString action = a.value("action").toString();
    if (type == "page") {
        if (action == "next") m_profiles.nextPage(1);
        else if (action == "prev") m_profiles.nextPage(-1);
        else m_profiles.setPage(a.value("page").toInt());
        toast(m_profiles.activeName(), m_profiles.pageName());
    } else if (type == "profile") {
        if (action == "auto") m_profiles.unpin();
        else m_profiles.pin(a.value("name").toString());
        toast(tr("Profile"), m_profiles.activeName());
    } else if (type == "haptics") {
        const bool target = action == "on" ? true : action == "off" ? false : m_haptics != 1;
        m_config.setDevice("haptics", target, nullptr);
    }
}

QString Daemon::controlSummary(const QJsonObject &a) const {
    const QString type = a.value("type").toString();
    if (type == "keys") {
        if (a.contains("omarchy")) return QStringLiteral("Omarchy shortcut \"%1\"").arg(a.value("omarchy").toString());
        return a.value("combo").toString() + (a.value("via").toString() == "hypr" ? " (hypr)" : "");
    }
    QStringList parts{type};
    for (const char *k : {"action", "dispatch", "arg", "command", "app", "url", "name", "text", "page"}) {
        const QJsonValue v = a.value(k);
        if (v.isString() && !v.toString().isEmpty()) parts << v.toString();
        else if (v.isDouble()) parts << QString::number(v.toInt());
    }
    return parts.join(' ');
}

// ---------------------------------------------------------------------------
// Hyprland shortcut links

QJsonArray Daemon::loadHyprBinds(bool force) {
    if (!force && m_bindCacheAge.isValid() && m_bindCacheAge.elapsed() < 5000) return m_bindCache;
    QProcess process;
    process.start("hyprctl", {"-j", "binds"});
    QJsonArray result;
    if (process.waitForFinished(2000) && process.exitCode() == 0) {
        for (const HyprBind &b : Shortcut::parseHyprBinds(process.readAllStandardOutput())) {
            if (b.description.isEmpty()) continue;
            result.append(QJsonObject{{"description", b.description}, {"combo", b.combo}, {"usable", b.usable}});
        }
        m_bindCache = result;
        m_bindCacheAge.start();
    }
    return m_bindCache;
}

QString Daemon::resolveOmarchy(const QJsonObject &action) {
    const QString wanted = action.value("omarchy").toString();
    for (const auto &item : loadHyprBinds(false)) {
        const QJsonObject b = item.toObject();
        if (b.value("usable").toBool() && b.value("description").toString() == wanted) return b.value("combo").toString();
    }
    return {};
}

// Re-resolves every Omarchy link by description: a changed combo is used going
// forward, and a vanished description is reported in status instead of failing silently.
void Daemon::checkLinks() {
    m_warnings.clear();
    if (!m_tools.has("hyprctl")) return;
    bool any = false;
    for (const auto &p : m_config.profiles())
        for (const auto &pg : p.toObject().value("pages").toArray())
            for (const auto &c : pg.toObject().value("bindings").toObject())
                for (const auto &s : c.toObject())
                    any = any || s.toObject().contains("omarchy");
    if (!any) return;
    const QJsonArray binds = loadHyprBinds(true);
    for (const auto &p : m_config.profiles())
        for (const auto &pg : p.toObject().value("pages").toArray())
            for (const auto &c : pg.toObject().value("bindings").toObject())
                for (const auto &s : c.toObject()) {
                    const QJsonObject a = s.toObject();
                    if (!a.contains("omarchy")) continue;
                    bool found = false;
                    for (const auto &b : binds)
                        found = found || b.toObject().value("description").toString() == a.value("omarchy").toString();
                    if (!found)
                        m_warnings << QStringLiteral("Omarchy shortcut \"%1\" no longer exists in Hyprland").arg(a.value("omarchy").toString());
                }
}

// ---------------------------------------------------------------------------
// IPC

QJsonObject Daemon::status() const {
    const int pct = m_link.batteryPercent() >= 0 ? m_link.batteryPercent() : m_reportPercent;
    QString serial;
    for (auto it = m_serialParts.begin(); it != m_serialParts.end(); ++it) serial += it.value();
    auto opt = [](int v) { return v < 0 ? QJsonValue(QJsonValue::Null) : QJsonValue(v); };
    return {
        {"ok", true},
        {"version", DIALSTUDIO_VERSION},
        {"uptime_s", int(m_uptime.elapsed() / 1000)},
        {"link", BluezLink::stateName(m_link.state())},
        {"device", QJsonObject{{"name", m_link.deviceName()}, {"address", m_link.deviceAddress()},
                               {"connected", m_link.deviceConnected()}, {"firmware", m_firmware},
                               {"custom_code", m_customCode}, {"serial", serial}}},
        {"battery", QJsonObject{{"percent", opt(pct)}, {"charging", m_charging}, {"full", m_full},
                                {"millivolts", opt(m_millivolts > 0 ? m_millivolts : -1)}}},
        {"haptics", m_haptics < 0 ? QJsonValue(QJsonValue::Null) : QJsonValue(m_haptics == 1)},
        {"sleep_seconds", opt(m_sleep)},
        {"profile", QJsonObject{{"active", m_profiles.activeName()}, {"pinned", m_profiles.pinned()},
                                {"page", m_profiles.page()}, {"page_name", m_profiles.pageName()},
                                {"pages", m_profiles.pageCount()}}},
        {"window", QJsonObject{{"class", m_profiles.windowClass()}, {"title", m_profiles.windowTitle()}}},
        {"config", QJsonObject{{"path", m_config.path()}, {"error", m_configError}}},
        {"warnings", QJsonArray::fromStringList(m_warnings)},
        {"last_error", m_lastError},
    };
}

QJsonObject Daemon::opBind(const QJsonObject &r) {
    ControlRef ref;
    QString error;
    if (!Config::parseControl(r.value("control").toString(), r.value("on").toString(), &ref, &error)) return err(error);

    QJsonObject action = r.value("action").toObject();
    if (action.isEmpty()) return err("bind needs an action");
    if (action.value("type").toString() == "keys" && action.contains("omarchy")) {
        // Link to an existing Hyprland bind: resolve now so typos fail at bind time, and
        // cache the combo so the link still works if the description disappears later.
        const QString wanted = action.value("omarchy").toString();
        QString description, combo;
        int partials = 0;
        for (const auto &item : loadHyprBinds(true)) {
            const QJsonObject b = item.toObject();
            if (!b.value("usable").toBool()) continue;
            const QString d = b.value("description").toString();
            if (d == wanted) { description = d; combo = b.value("combo").toString(); partials = 1; break; }
            if (d.contains(wanted, Qt::CaseInsensitive)) {
                if (partials++ == 0) { description = d; combo = b.value("combo").toString(); }
            }
        }
        if (description.isEmpty())
            return err(QStringLiteral("no Hyprland shortcut with a description matching \"%1\" (see: dialstudio shortcuts)").arg(wanted));
        if (partials > 1)
            return err(QStringLiteral("\"%1\" matches several shortcuts; use the full description").arg(wanted));
        action.insert("omarchy", description);
        action.insert("combo", combo);
        if (!action.contains("via")) action.insert("via", "hypr");
    }
    const QString profile = r.value("profile").toString().isEmpty() ? m_config.defaultProfileName() : r.value("profile").toString();
    if (!m_config.bind(profile, r.value("page").toInt(0), ref, action, &error)) return err(error);
    return ok({{"profile", profile}, {"control", ref.control}, {"slot", ref.slot}, {"action", action}});
}

QJsonObject Daemon::opUnbind(const QJsonObject &r) {
    ControlRef ref;
    QString error;
    const QString text = r.value("control").toString();
    const QString on = r.value("on").toString();
    // A bare control name with no --on removes every slot of that control.
    const bool bare = !text.contains('.') && on.isEmpty();
    if (bare) {
        ref.control = text;
        if (!Config::keyControls().contains(text) && text != "knob") return err(QStringLiteral("unknown control '%1' (key1..key7 or knob)").arg(text));
    } else if (!Config::parseControl(text, on, &ref, &error)) {
        return err(error);
    }
    const QString profile = r.value("profile").toString().isEmpty() ? m_config.defaultProfileName() : r.value("profile").toString();
    if (!m_config.unbind(profile, r.value("page").toInt(0), ref, bare, &error)) return err(error);
    return ok({{"profile", profile}, {"control", ref.control}});
}

QJsonObject Daemon::opBindings(const QJsonObject &r) const {
    QJsonArray rows;
    const QString only = r.value("profile").toString();
    for (const auto &p : m_config.profiles()) {
        const QJsonObject profile = p.toObject();
        if (!only.isEmpty() && profile.value("name").toString() != only) continue;
        int index = 0;
        for (const auto &pg : profile.value("pages").toArray()) {
            const QJsonObject bindings = pg.toObject().value("bindings").toObject();
            QStringList controls = Config::keyControls();
            controls << "knob";
            for (const QString &control : controls) {
                const QJsonObject slotMap = bindings.value(control).toObject();
                for (auto s = slotMap.begin(); s != slotMap.end(); ++s)
                    rows.append(QJsonObject{{"profile", profile.value("name")}, {"page", index}, {"control", control},
                                            {"slot", s.key()}, {"action", s.value()},
                                            {"summary", controlSummary(s.value().toObject())}});
            }
            ++index;
        }
    }
    return ok({{"bindings", rows}});
}

QJsonObject Daemon::opShortcuts() {
    if (!m_tools.has("hyprctl")) return err("hyprctl is not installed");
    return ok({{"shortcuts", loadHyprBinds(true)}});
}

QJsonObject Daemon::opSend(const QJsonObject &r) {
    const QJsonObject action = r.value("action").toObject();
    if (action.isEmpty()) return err("send needs an action");
    QString error;
    if (!Actions::validate(action, &error)) return err(error);
    // Built here so the reply can carry the exact argv that ran, which makes `send` useful for debugging.
    const QList<Command> commands = m_actions.build(action, 1, &error);
    if (!error.isEmpty()) return err(error);
    m_actions.run(action, 1);
    QJsonArray argv;
    for (const auto &c : commands) argv.append(QJsonArray::fromStringList(QStringList{c.program} + c.args));
    return ok({{"argv", argv}});
}

QJsonObject Daemon::opSet(const QJsonObject &r, const IpcResponder &respond, bool *deferred) {
    const QString what = r.value("what").toString();
    QString error;
    *deferred = false;
    const bool online = m_link.state() == BluezLink::State::Online;

    if (what == "haptics") {
        const bool on = r.value("value").toBool();
        if (!m_config.setDevice("haptics", on, &error)) return err(error);
        if (!online)
            return ok({{"haptics", on}, {"applied", false}, {"note", "dial not connected; applied on next connect"}});
        // onConfigChanged already sent the write when the dial disagreed. Otherwise
        // confirm with a read. Either way the reply is the dial's own answer.
        if (m_haptics == int(on)) m_link.send(Protocol::readHaptics());
        m_hapticsWaiters << respond;
        *deferred = true;
        QTimer::singleShot(2500, this, [this, respond] {
            if (m_hapticsWaiters.isEmpty()) return;
            m_hapticsWaiters.clear();
            respond(err("the dial did not answer within 2.5 s"));
        });
        return {};
    }
    if (what == "sleep") {
        const int seconds = r.value("value").toInt(-1);
        if (seconds < 0 || seconds > 65535) return err("sleep must be 0..65535 seconds (0 disables it)");
        if (!m_config.setDevice("sleep_seconds", seconds, &error)) return err(error);
        if (!online)
            return ok({{"sleep_seconds", seconds}, {"applied", false}, {"note", "dial not connected; applied on next connect"}});
        if (m_sleep == seconds) m_link.send(Protocol::readSleep());
        m_sleepWaiters << respond;
        *deferred = true;
        QTimer::singleShot(2500, this, [this, respond] {
            if (m_sleepWaiters.isEmpty()) return;
            m_sleepWaiters.clear();
            respond(err("the dial did not answer within 2.5 s"));
        });
        return {};
    }
    return err("set: unknown setting '" + what + "' (haptics, sleep)");
}

void Daemon::handle(const QJsonObject &r, const IpcResponder &respond) {
    const QString op = r.value("op").toString();
    if (op == "status") { respond(status()); return; }
    if (op == "reload") { m_config.reload(); respond(ok()); return; }
    if (op == "profile") {
        const QString name = r.value("name").toString();
        if (name == "auto") { m_profiles.unpin(); respond(ok({{"profile", m_profiles.activeName()}})); return; }
        if (!m_profiles.pin(name)) { respond(err(QStringLiteral("no profile named '%1'").arg(name))); return; }
        toast(tr("Profile"), name);
        respond(ok({{"profile", name}}));
        return;
    }
    if (op == "page") {
        const QString which = r.value("page").toString();
        bool good;
        if (which == "next") good = m_profiles.nextPage(1);
        else if (which == "prev") good = m_profiles.nextPage(-1);
        else {
            bool number = false;
            const int n = which.toInt(&number);
            good = number && m_profiles.setPage(n);
        }
        if (!good) { respond(err(QStringLiteral("page must be next, prev or 0..%1").arg(m_profiles.pageCount() - 1))); return; }
        toast(m_profiles.activeName(), m_profiles.pageName());
        respond(ok({{"page", m_profiles.page()}, {"page_name", m_profiles.pageName()}}));
        return;
    }
    if (op == "set") {
        bool deferred = false;
        const QJsonObject reply = opSet(r, respond, &deferred);
        if (!deferred) respond(reply);
        return;
    }
    if (op == "bind") { respond(opBind(r)); return; }
    if (op == "unbind") { respond(opUnbind(r)); return; }
    if (op == "bindings") { respond(opBindings(r)); return; }
    if (op == "shortcuts") { respond(opShortcuts()); return; }
    if (op == "send") { respond(opSend(r)); return; }
    respond(err(QStringLiteral("unknown op '%1'").arg(op)));
}
