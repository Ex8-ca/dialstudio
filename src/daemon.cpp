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
    m_actions.setOmarchyResolver([this](const QString &description, const QString &combo) {
        return lookupShortcut(description, combo);
    });
    connect(&m_scan, &OmarchyScan::finished, this, &Daemon::onScanFinished);
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
        // The text is what `dialstudio events` prints; the fields are for the GUI.
        QJsonObject event{{"event", "input"}, {"text", text}};
        const QStringList parts = text.split(' ');
        if (parts.size() >= 2 && parts[0].startsWith("key")) {
            event.insert("control", parts[0]);
            event.insert("kind", parts[1]);
        } else if (text == "knob click") {
            event.insert("control", "knob");
            event.insert("kind", "click");
        } else if (parts.size() >= 3 && (parts[0] == "rotate" || parts[0] == "press_rotate")) {
            event.insert("control", "knob");
            event.insert("kind", parts[0]);
            event.insert("direction", parts[1]);
            event.insert("steps", parts[2].section('=', 1).toInt());
        }
        publish(event);
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
    connect(&m_hypr, &HyprlandWatcher::configReloaded, this, &Daemon::refreshShortcuts);
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
    refreshShortcuts();
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
    if (type == "omarchy_bind" || (type == "keys" && a.contains("omarchy")))
        return QStringLiteral("Omarchy: %1").arg(type == "keys" ? a.value("omarchy").toString() : a.value("description").toString());
    if (type == "keys") {
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
// Omarchy shortcut links
//
// The catalog comes from scanning Hyprland's Lua config (see omarchybinds.h). A linked
// control stores the bind's description; every time it fires, the description is looked up
// again, so a remapped key keeps working and a removed shortcut is reported instead of
// silently doing nothing.

void Daemon::refreshShortcuts() {
    if (m_options.hyprConfig.isEmpty()) return;
    if (m_scan.running()) { m_rescanQueued = true; return; }
    m_scan.start(m_options.hyprConfig, m_options.omarchyPath.isEmpty() ? OmarchyScan::defaultOmarchyPath() : m_options.omarchyPath);
}

void Daemon::onScanFinished(bool ok, const QList<OmarchyBind> &binds, const QString &error) {
    if (ok || !binds.isEmpty()) {
        m_catalog.set(binds);
        m_scanError = ok ? QString() : error;
    } else {
        m_scanError = error;
        qWarning().noquote() << "omarchy shortcut scan failed:" << error;
    }
    checkLinks();
    publish({{"event", "shortcuts"}, {"count", int(m_catalog.binds().size())}});
    if (m_rescanQueued) { m_rescanQueued = false; refreshShortcuts(); }
}

// A bind or `shortcuts` call can arrive before the first scan finishes; the scan takes
// about 30 ms, so waiting for it here is cheaper than answering "not found".
void Daemon::ensureCatalog() {
    if (m_catalog.loaded() || m_options.hyprConfig.isEmpty()) return;
    QList<OmarchyBind> binds;
    QString error;
    const bool ok = OmarchyScan::run(m_options.hyprConfig, m_options.omarchyPath.isEmpty() ? OmarchyScan::defaultOmarchyPath() : m_options.omarchyPath,
                                     &binds, &error, 3000);
    if (ok || !binds.isEmpty()) m_catalog.set(binds);
    m_scanError = ok ? QString() : error;
}

OmarchyLookup Daemon::lookupShortcut(const QString &description, const QString &combo) {
    ensureCatalog();
    OmarchyLookup found = m_catalog.find(description, combo);
    if (found.status == OmarchyLookup::Status::NotFound && !m_scanError.isEmpty() && !m_catalog.loaded())
        found.message = QStringLiteral("cannot read the Omarchy shortcuts: %1").arg(m_scanError);
    return found;
}

// Re-resolves every Omarchy link by description and reports the ones that went away or
// can no longer run, in `status` and the GUI.
void Daemon::checkLinks() {
    m_warnings.clear();
    if (!m_catalog.loaded()) return;
    for (const auto &p : m_config.profiles())
        for (const auto &pg : p.toObject().value("pages").toArray())
            for (const auto &c : pg.toObject().value("bindings").toObject())
                for (auto s = c.toObject().begin(); s != c.toObject().end(); ++s) {
                    const QJsonObject a = s.value().toObject();
                    const bool legacy = a.value("type").toString() == "keys" && a.contains("omarchy");
                    if (a.value("type").toString() != "omarchy_bind" && !legacy) continue;
                    const QString description = legacy ? a.value("omarchy").toString() : a.value("description").toString();
                    const OmarchyLookup found = m_catalog.find(description, a.value("combo").toString());
                    if (found.status == OmarchyLookup::Status::NotFound)
                        m_warnings << QStringLiteral("Omarchy shortcut \"%1\" no longer exists in the Hyprland config").arg(description);
                    else if (found.status == OmarchyLookup::Status::Found && !found.bind.runnable)
                        m_warnings << QStringLiteral("Omarchy shortcut \"%1\" can no longer be run: %2").arg(description, found.bind.reason);
                    else if (found.status == OmarchyLookup::Status::Ambiguous)
                        m_warnings << found.message;
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
        {"shortcuts", QJsonObject{{"loaded", m_catalog.loaded()}, {"total", int(m_catalog.binds().size())},
                                  {"runnable", m_catalog.runnableCount()}, {"error", m_scanError}}},
        {"last_error", m_lastError},
    };
}

QJsonObject Daemon::opBind(const QJsonObject &r) {
    ControlRef ref;
    QString error;
    if (!Config::parseControl(r.value("control").toString(), r.value("on").toString(), &ref, &error)) return err(error);

    QJsonObject action = r.value("action").toObject();
    if (action.isEmpty()) return err("bind needs an action");
    if (action.value("type").toString() == "omarchy_bind" || (action.value("type").toString() == "keys" && action.contains("omarchy"))) {
        // Resolve now so a typo, an ambiguous name or a bind that cannot be run fails at
        // bind time with a clear message, not silently when the dial is pressed.
        const QString wanted = action.contains("omarchy") ? action.value("omarchy").toString() : action.value("description").toString();
        const OmarchyLookup found = lookupShortcut(wanted, action.value("combo").toString());
        if (found.status != OmarchyLookup::Status::Found) return err(found.message);
        if (!found.bind.runnable)
            return err(QStringLiteral("\"%1\" (%2) cannot be bound: %3").arg(found.bind.description, found.bind.combo(), found.bind.reason));
        action = QJsonObject{{"type", "omarchy_bind"}, {"description", found.bind.description}, {"combo", found.bind.combo()}};
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

QJsonObject Daemon::opShortcuts(const QJsonObject &r) {
    if (m_options.hyprConfig.isEmpty()) return err("the Omarchy shortcut scan is disabled");
    if (r.value("refresh").toBool()) {
        m_catalog.clear();
    }
    ensureCatalog();
    if (!m_catalog.loaded()) return err(m_scanError.isEmpty() ? QStringLiteral("could not read the Omarchy shortcuts") : m_scanError);
    const QString filter = r.value("filter").toString();
    QJsonArray rows;
    for (const OmarchyBind &b : m_catalog.binds())
        if (filter.isEmpty() || b.description.contains(filter, Qt::CaseInsensitive) || b.combo().contains(filter, Qt::CaseInsensitive))
            rows.append(b.toJson());
    return ok({{"shortcuts", rows}, {"runnable", m_catalog.runnableCount()}, {"total", int(m_catalog.binds().size())},
               {"warning", m_scanError}});
}

// The whole bindings object of one page, plus the profile and page lists, so a client can
// show and edit without parsing the config file itself.
QJsonObject Daemon::opBindingsGet(const QJsonObject &r) const {
    QString profile = r.value("profile").toString();
    if (profile.isEmpty()) profile = m_config.defaultProfileName();
    const int p = m_config.profileIndex(profile);
    if (p < 0) return err(QStringLiteral("no profile named '%1'").arg(profile));
    const QJsonArray pages = m_config.profiles()[p].toObject().value("pages").toArray();
    const int page = r.value("page").toInt(0);
    if (page < 0 || page >= pages.size()) return err(QStringLiteral("profile '%1' has no page %2").arg(profile).arg(page));
    QJsonArray names;
    for (const auto &pg : pages) names.append(pg.toObject().value("name").toString());
    return ok({{"profile", profile}, {"page", page}, {"page_names", names},
               {"bindings", pages[page].toObject().value("bindings").toObject()},
               {"device", m_config.device()}});
}

// Replaces one page's bindings in one validated, atomic save. This is how the GUI applies
// an edit and how undo/redo restores a snapshot.
QJsonObject Daemon::opBindingsSet(const QJsonObject &r) {
    QString profile = r.value("profile").toString();
    if (profile.isEmpty()) profile = m_config.defaultProfileName();
    const int p = m_config.profileIndex(profile);
    if (p < 0) return err(QStringLiteral("no profile named '%1'").arg(profile));
    QJsonArray profileList = m_config.profiles();
    QJsonObject prof = profileList[p].toObject();
    QJsonArray pages = prof.value("pages").toArray();
    const int page = r.value("page").toInt(0);
    if (page < 0 || page >= pages.size()) return err(QStringLiteral("profile '%1' has no page %2").arg(profile).arg(page));
    if (!r.value("bindings").isObject()) return err("bindings_set needs a bindings object");
    QJsonObject pg = pages[page].toObject();
    pg.insert("bindings", r.value("bindings").toObject());
    pages[page] = pg;
    prof.insert("pages", pages);
    profileList[p] = prof;
    QJsonObject doc = m_config.doc();
    doc.insert("profiles", profileList);
    // Same validation as the config file: a bad slot or action is refused, nothing is written.
    QString error;
    QStringList problems;
    if (!Config::validate(doc, &problems)) return err(problems.join("; "));
    if (!m_config.replace(doc, &error)) return err(error);
    return opBindingsGet(r);
}

// Development hook for tests and screenshots: feeds a decoded dial event into the input
// pipeline as if it had come from the dial. Off unless DIALSTUDIO_DEV=1 is set for the daemon.
QJsonObject Daemon::opInject(const QJsonObject &r) {
    if (qEnvironmentVariable("DIALSTUDIO_DEV") != "1") return err("inject is only available when the daemon runs with DIALSTUDIO_DEV=1");
    const QString kind = r.value("kind").toString();
    const int key = r.value("key").toInt();
    if (kind == "down" || kind == "up") {
        if (key < 1 || key > 7) return err("key must be 1..7");
        if (kind == "down") m_input.keyDown(key);
        else m_input.keyUp(key);
    } else if (kind == "click") {
        m_input.knobClick();
    } else if (kind == "rotate") {
        const int dir = r.value("direction").toString() == "ccw" ? -1 : 1;
        for (int i = 0, n = qBound(1, r.value("steps").toInt(1), 20); i < n; ++i)
            m_input.rotate(dir, r.value("pressed").toBool());
    } else {
        return err("inject kind must be down, up, click or rotate");
    }
    return ok();
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
    if (op == "shortcuts") { respond(opShortcuts(r)); return; }
    if (op == "bindings_get") { respond(opBindingsGet(r)); return; }
    if (op == "bindings_set") { respond(opBindingsSet(r)); return; }
    if (op == "send" || op == "test") { respond(opSend(r)); return; }
    if (op == "inject") { respond(opInject(r)); return; }
    respond(err(QStringLiteral("unknown op '%1'").arg(op)));
}
