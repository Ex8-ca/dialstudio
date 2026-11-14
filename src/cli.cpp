#include "cli.h"
#include "version.h"

#include "actions.h"
#include "bluezlink.h"
#include "config.h"
#include "daemon.h"
#include "ipc.h"
#include "omarchybinds.h"
#include "shortcut.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMetaType>
#include <QDBusPendingReply>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QStandardPaths>
#include <QTextStream>
#include <cstdio>
#include <termios.h>
#include <unistd.h>

namespace {

QTextStream out(stdout);
QTextStream errs(stderr);

const char *Usage = R"(dialstudio %1: Ulanzi D100H controller

  dialstudio daemon                      run the headless daemon (systemd runs this)
  dialstudio status [--json]             device, battery, firmware, haptics, sleep, profile
  dialstudio events [--json]             stream decoded dial events
  dialstudio set haptics on|off          write the dial's haptics setting
  dialstudio set sleep <seconds>         write the dial's sleep timer (0 disables)
  dialstudio profile <name>|auto         pin a profile, or go back to window matching
  dialstudio page next|prev|<n>          switch page of the active profile
  dialstudio doctor [--json]             check bluez, pairing, tools, config, service

  dialstudio bind <control> <combo> [--on press|long_press|release] [--profile NAME]
                                    [--page N] [--via wtype|hypr]
  dialstudio bind <control> --omarchy "Volume up"     run an existing Omarchy shortcut's command
  dialstudio bind <control> --action '{"type":"exec","command":"foot"}'
  dialstudio unbind <control> [--on SLOT] [--profile NAME] [--page N]
  dialstudio bindings [--json] [--profile NAME]
  dialstudio send <combo> [--via wtype|hypr]          fire a shortcut now
  dialstudio shortcuts [--json] [FILTER]              list Omarchy shortcuts and whether they can run
  dialstudio learn <control> [--on SLOT]              press a combo on the keyboard to bind it

controls: key1..key7 (press, long_press, release), knob.click, knob.rotate_cw/ccw,
          knob.press_rotate_cw/ccw, knob.rotate_cw@key4 (rotate while key4 is held)
)";

struct Args {
    QStringList positional;
    QHash<QString, QString> options;
    QSet<QString> flags;
    bool bad = false;
    QString problem;
};

// Options that take a value; everything else starting with -- is a flag.
Args parse(const QStringList &args, const QStringList &valued) {
    Args result;
    for (int i = 0; i < args.size(); ++i) {
        const QString &a = args[i];
        if (a.startsWith("--") && a.size() > 2) {
            const QString name = a.mid(2);
            if (valued.contains(name)) {
                if (i + 1 >= args.size()) { result.bad = true; result.problem = "--" + name + " needs a value"; return result; }
                result.options.insert(name, args[++i]);
            } else {
                result.flags.insert(name);
            }
        } else {
            result.positional << a;
        }
    }
    return result;
}

int fail(const QString &message) {
    errs << "dialstudio: " << message << "\n";
    return 1;
}

bool connectDaemon(IpcClient &client) {
    if (client.connectTo(Ipc::socketPath())) return true;
    errs << "dialstudio: the daemon is not running.\n"
            "  start it with: systemctl --user start dialstudio   (or: dialstudio daemon)\n";
    return false;
}

// One request, one reply; prints the daemon's error on failure.
bool call(const QJsonObject &request, QJsonObject *reply) {
    IpcClient client;
    if (!connectDaemon(client)) return false;
    QString error;
    *reply = client.request(request, 4000, &error);
    if (reply->isEmpty()) { errs << "dialstudio: " << error << "\n"; return false; }
    if (!reply->value("ok").toBool()) {
        errs << "dialstudio: " << reply->value("error").toString() << "\n";
        return false;
    }
    return true;
}

void printJson(const QJsonObject &o) { out << QJsonDocument(o).toJson(QJsonDocument::Indented); }

QString yesNo(const QJsonValue &v, const QString &yes, const QString &no) {
    return v.isNull() || v.isUndefined() ? QStringLiteral("unknown") : (v.toBool() ? yes : no);
}

int cmdStatus(const Args &a) {
    QJsonObject s;
    if (!call({{"op", "status"}}, &s)) return 1;
    if (a.flags.contains("json")) { printJson(s); return 0; }
    const QJsonObject d = s.value("device").toObject(), b = s.value("battery").toObject();
    const QJsonObject p = s.value("profile").toObject(), w = s.value("window").toObject();
    out << "link:      " << s.value("link").toString() << "\n";
    out << "device:    " << d.value("name").toString() << " " << d.value("address").toString() << "\n";
    out << "firmware:  " << (d.value("firmware").toString().isEmpty() ? "unknown" : d.value("firmware").toString()) << "\n";
    out << "battery:   " << (b.value("percent").isNull() ? "unknown" : QString::number(b.value("percent").toInt()) + "%")
        << (b.value("charging").toBool() ? " (charging)" : "") << "\n";
    out << "haptics:   " << yesNo(s.value("haptics"), "on", "off") << "\n";
    out << "sleep:     " << (s.value("sleep_seconds").isNull() ? "unknown" : QString::number(s.value("sleep_seconds").toInt()) + " s") << "\n";
    out << "profile:   " << p.value("active").toString() << (p.value("pinned").toBool() ? " (pinned)" : "")
        << ", page " << p.value("page").toInt() << " \"" << p.value("page_name").toString() << "\"\n";
    out << "window:    " << w.value("class").toString() << "\n";
    for (const auto &warning : s.value("warnings").toArray()) out << "warning:   " << warning.toString() << "\n";
    if (!s.value("config").toObject().value("error").toString().isEmpty())
        out << "config:    " << s.value("config").toObject().value("error").toString() << "\n";
    if (!s.value("last_error").toString().isEmpty()) out << "last error: " << s.value("last_error").toString() << "\n";
    return 0;
}

int cmdEvents(const Args &a) {
    IpcClient client;
    if (!connectDaemon(client)) return 1;
    QString error;
    const QJsonObject ack = client.request({{"op", "subscribe"}}, 3000, &error);
    if (!ack.value("ok").toBool()) return fail(error.isEmpty() ? "subscribe failed" : error);
    const bool json = a.flags.contains("json");
    for (;;) {
        QJsonObject event;
        bool closed = false;
        if (!client.readLine(&event, -1, &closed)) {
            if (closed) return fail("the daemon closed the connection");
            continue;
        }
        const QString kind = event.value("event").toString();
        if (json) {
            out << QJsonDocument(event).toJson(QJsonDocument::Compact) << "\n";
        } else if (kind == "input") {
            out << event.value("text").toString() << "\n";
        } else if (kind == "link") {
            out << "# link " << event.value("state").toString() << "\n";
        } else if (kind == "profile") {
            out << "# profile " << event.value("profile").toString() << " page " << event.value("page").toInt() << "\n";
        } else if (kind == "battery") {
            out << "# battery " << event.value("percent").toInt() << "%\n";
        } else if (kind == "error") {
            out << "# error " << event.value("action").toString() << ": " << event.value("message").toString() << "\n";
        } else if (kind == "config_error") {
            out << "# config error " << event.value("message").toString() << "\n";
        } else if (kind == "config") {
            out << "# config reloaded\n";
        }
        out.flush();
    }
}

int cmdSet(const Args &a) {
    if (a.positional.size() != 2) return fail("usage: dialstudio set haptics on|off | set sleep <seconds>");
    const QString what = a.positional[0], value = a.positional[1];
    QJsonObject request{{"op", "set"}, {"what", what}};
    if (what == "haptics") {
        if (value != "on" && value != "off") return fail("haptics takes on or off");
        request.insert("value", value == "on");
    } else if (what == "sleep") {
        bool number = false;
        const int seconds = value.toInt(&number);
        if (!number || seconds < 0 || seconds > 65535) return fail("sleep takes a number of seconds, 0..65535");
        request.insert("value", seconds);
    } else {
        return fail("unknown setting '" + what + "' (haptics, sleep)");
    }
    QJsonObject reply;
    if (!call(request, &reply)) return 1;
    if (what == "haptics") out << "haptics " << (reply.value("haptics").toBool() ? "on" : "off");
    else out << "sleep " << reply.value("sleep_seconds").toInt() << " s";
    out << (reply.value("applied").toBool(true) ? " (confirmed by the dial)" : " (" + reply.value("note").toString() + ")") << "\n";
    return 0;
}

int cmdProfile(const Args &a) {
    if (a.positional.size() != 1) return fail("usage: dialstudio profile <name>|auto");
    QJsonObject reply;
    if (!call({{"op", "profile"}, {"name", a.positional[0]}}, &reply)) return 1;
    out << "profile " << reply.value("profile").toString() << "\n";
    return 0;
}

int cmdPage(const Args &a) {
    if (a.positional.size() != 1) return fail("usage: dialstudio page next|prev|<n>");
    QJsonObject reply;
    if (!call({{"op", "page"}, {"page", a.positional[0]}}, &reply)) return 1;
    out << "page " << reply.value("page").toInt() << " \"" << reply.value("page_name").toString() << "\"\n";
    return 0;
}

QJsonObject keysAction(const QString &combo, const QString &via) {
    QJsonObject action{{"type", "keys"}, {"combo", combo}};
    if (!via.isEmpty()) action.insert("via", via);
    return action;
}

int cmdBind(const Args &a) {
    if (a.positional.isEmpty()) return fail("usage: dialstudio bind <control> <combo>  (see dialstudio --help)");
    QJsonObject action;
    const QString via = a.options.value("via");
    if (a.options.contains("action")) {
        QJsonParseError e;
        const QJsonDocument doc = QJsonDocument::fromJson(a.options.value("action").toUtf8(), &e);
        if (!doc.isObject()) return fail("--action is not a JSON object: " + e.errorString());
        action = doc.object();
    } else if (a.options.contains("omarchy")) {
        // A reference by description: the daemon looks the shortcut up in Omarchy's own
        // config and runs its command (or Hyprland dispatch), never types the keys.
        action = {{"type", "omarchy_bind"}, {"description", a.options.value("omarchy")}};
    } else {
        if (a.positional.size() != 2) return fail("bind needs a combo, --omarchy \"description\" or --action JSON");
        // Reject typos here too so the message appears even before the daemon is asked.
        Combo combo;
        QString error;
        if (!Shortcut::parse(a.positional[1], &combo, &error)) return fail(error);
        action = keysAction(combo.toString(), via);
    }
    QJsonObject reply;
    QJsonObject request{{"op", "bind"}, {"control", a.positional[0]}, {"action", action},
                        {"page", a.options.value("page", "0").toInt()}};
    if (a.options.contains("on")) request.insert("on", a.options.value("on"));
    if (a.options.contains("profile")) request.insert("profile", a.options.value("profile"));
    if (!call(request, &reply)) return 1;
    out << "bound " << reply.value("control").toString() << "." << reply.value("slot").toString()
        << " in profile " << reply.value("profile").toString() << "\n";
    return 0;
}

int cmdUnbind(const Args &a) {
    if (a.positional.size() != 1) return fail("usage: dialstudio unbind <control> [--on SLOT]");
    QJsonObject request{{"op", "unbind"}, {"control", a.positional[0]}, {"page", a.options.value("page", "0").toInt()}};
    if (a.options.contains("on")) request.insert("on", a.options.value("on"));
    if (a.options.contains("profile")) request.insert("profile", a.options.value("profile"));
    QJsonObject reply;
    if (!call(request, &reply)) return 1;
    out << "unbound " << reply.value("control").toString() << "\n";
    return 0;
}

int cmdBindings(const Args &a) {
    QJsonObject request{{"op", "bindings"}};
    if (a.options.contains("profile")) request.insert("profile", a.options.value("profile"));
    QJsonObject reply;
    if (!call(request, &reply)) return 1;
    if (a.flags.contains("json")) { printJson(reply); return 0; }
    for (const auto &item : reply.value("bindings").toArray()) {
        const QJsonObject b = item.toObject();
        out << QString("%1 p%2  %3.%4").arg(b.value("profile").toString(), -10).arg(b.value("page").toInt())
                   .arg(b.value("control").toString(), b.value("slot").toString()).leftJustified(40)
            << " " << b.value("summary").toString() << "\n";
    }
    return 0;
}

int cmdSend(const Args &a) {
    if (a.positional.size() != 1) return fail("usage: dialstudio send <combo> [--via wtype|hypr]");
    Combo combo;
    QString error;
    if (!Shortcut::parse(a.positional[0], &combo, &error)) return fail(error);
    QJsonObject reply;
    if (!call({{"op", "send"}, {"action", keysAction(combo.toString(), a.options.value("via"))}}, &reply)) return 1;
    for (const auto &argv : reply.value("argv").toArray()) {
        QStringList parts;
        for (const auto &p : argv.toArray()) parts << p.toString();
        out << "ran: " << parts.join(' ') << "\n";
    }
    return 0;
}

int cmdShortcuts(const Args &a) {
    const QString filter = a.positional.value(0);
    QJsonArray rows;
    int runnable = 0, total = 0;
    QString warning;
    // Prefer the daemon's catalog; without it, scan the config here (read only).
    IpcClient client;
    if (client.connectTo(Ipc::socketPath(), 300)) {
        QString error;
        const QJsonObject reply = client.request({{"op", "shortcuts"}, {"filter", filter}}, 6000, &error);
        if (reply.value("ok").toBool()) {
            rows = reply.value("shortcuts").toArray();
            runnable = reply.value("runnable").toInt();
            total = reply.value("total").toInt();
            warning = reply.value("warning").toString();
        } else if (!reply.isEmpty()) {
            return fail(reply.value("error").toString());
        }
    }
    if (rows.isEmpty() && total == 0) {
        QList<OmarchyBind> binds;
        QString error;
        if (!OmarchyScan::run(OmarchyScan::defaultConfig(), OmarchyScan::defaultOmarchyPath(), &binds, &error))
            if (binds.isEmpty()) return fail("could not read the Omarchy shortcuts: " + error);
        warning = error;
        OmarchyCatalog catalog;
        catalog.set(binds);
        total = binds.size();
        runnable = catalog.runnableCount();
        for (const OmarchyBind &b : binds)
            if (filter.isEmpty() || b.description.contains(filter, Qt::CaseInsensitive) || b.combo().contains(filter, Qt::CaseInsensitive))
                rows.append(b.toJson());
    }
    if (a.flags.contains("json")) { out << QJsonDocument(rows).toJson(QJsonDocument::Indented); return 0; }
    for (const auto &r : rows) {
        const QJsonObject o = r.toObject();
        out << o.value("combo").toString().leftJustified(34) << " " << o.value("description").toString().leftJustified(40);
        if (o.value("runnable").toBool()) out << " " << o.value("runs").toString() << "\n";
        else out << " (not runnable: " << o.value("reason").toString() << ")\n";
    }
    errs << rows.size() << " shown, " << runnable << " of " << total << " shortcuts can be run by dialstudio\n";
    if (!warning.isEmpty()) errs << "warning: " << warning << "\n";
    return 0;
}

// Reads one key press from the terminal in raw mode. A short drain after the first byte
// collects the rest of an escape sequence.
QByteArray readKeyPress() {
    termios saved{};
    if (tcgetattr(STDIN_FILENO, &saved) != 0) return {};
    termios raw = saved;
    cfmakeraw(&raw);
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    // Ask kitty-protocol terminals (foot, kitty, ghostty, alacritty) for unambiguous key reports.
    (void)!::write(STDOUT_FILENO, "\x1b[>1u", 5);
    QByteArray bytes;
    char c;
    if (::read(STDIN_FILENO, &c, 1) == 1) {
        bytes.append(c);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 1;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        while (::read(STDIN_FILENO, &c, 1) == 1) bytes.append(c);
    }
    (void)!::write(STDOUT_FILENO, "\x1b[<u", 4);
    tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    return bytes;
}

int cmdLearn(const Args &a) {
    if (a.positional.size() != 1) return fail("usage: dialstudio learn <control> [--on SLOT]");
    if (!isatty(STDIN_FILENO)) return fail("learn needs an interactive terminal");
    out << "Press the shortcut for " << a.positional[0] << " (Super is usually grabbed by Hyprland; type it instead with: dialstudio bind)... " << Qt::flush;
    const QByteArray bytes = readKeyPress();
    out << "\n";
    Combo combo;
    QString error;
    if (!Shortcut::fromTerminalInput(bytes, &combo, &error)) return fail(error);
    out << "got " << combo.toString() << "\n";
    Args bind;
    bind.positional << a.positional[0] << combo.toString();
    bind.options = a.options;
    return cmdBind(bind);
}

// ---------------------------------------------------------------------------
// doctor

struct Check {
    QString name;
    bool ok;
    QString detail;
    bool required = true;
};

QList<Check> runChecks() {
    QList<Check> checks;
    auto add = [&](const QString &name, bool ok, const QString &detail, bool required = true) {
        checks.append({name, ok, detail, required});
    };

    QDBusConnection bus = QDBusConnection::systemBus();
    BluezModel model;
    bool bluez = false;
    if (bus.isConnected()) {
        qDBusRegisterMetaType<BluezInterfaces>();
        qDBusRegisterMetaType<BluezObjects>();
        QDBusMessage call = QDBusMessage::createMethodCall("org.bluez", "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
        QDBusPendingReply<BluezObjects> reply = bus.call(call, QDBus::Block, 3000);
        if (!reply.isError()) { model.replaceAll(reply.value()); bluez = true; }
    }
    add("bluez", bluez, bluez ? "bluetoothd answers on the system bus" : "bluetoothd is not reachable (systemctl status bluetooth)");

    if (bluez) {
        const QString device = model.findDevice(QString());
        add("dial paired", !device.isEmpty(),
            device.isEmpty() ? "no Ulanzi Dial known to BlueZ: pair it with the Omarchy Bluetooth menu"
                             : model.property(device, "org.bluez.Device1", "Name").toString() + " " +
                                   model.property(device, "org.bluez.Device1", "Address").toString());
        if (!device.isEmpty()) {
            const QString adapter = model.adapterOf(device);
            add("adapter powered", model.property(adapter, "org.bluez.Adapter1", "Powered").toBool(), adapter);
            const auto prop = [&](const char *name) { return model.property(device, "org.bluez.Device1", name).toBool(); };
            add("paired / trusted", prop("Paired") && prop("Trusted"),
                QString("paired=%1 trusted=%2").arg(prop("Paired")).arg(prop("Trusted")));
            add("connected", prop("Connected"), prop("Connected") ? "yes" : "no (the daemon connects it; wake the dial)", false);
            const QString notify = model.characteristic(device, BluezUuid::Notify);
            const QString write = model.characteristic(device, BluezUuid::Write);
            add("vendor service 1971", !model.vendorServicePath(device).isEmpty(),
                model.vendorServicePath(device).isEmpty() ? "not resolved yet: connect the dial and retry" : model.vendorServicePath(device));
            add("characteristic fd00 (notify)", !notify.isEmpty(), notify);
            add("characteristic fd01 (write)", !write.isEmpty(), write);
        }
    }

    for (const char *tool : {"wpctl", "wtype", "hyprctl"})
        add(tool, !QStandardPaths::findExecutable(tool).isEmpty(), QStandardPaths::findExecutable(tool));
    for (const char *tool : {"brightnessctl", "notify-send", "playerctl", "swayosd-client", "uwsm-app", "systemd-run"})
        add(tool, !QStandardPaths::findExecutable(tool).isEmpty(),
            QStandardPaths::findExecutable(tool).isEmpty() ? "optional, not installed" : QStandardPaths::findExecutable(tool), false);

    Config config(Config::defaultPath());
    QFileInfo info(config.path());
    if (info.exists()) {
        QString error;
        add("config", config.load(&error), error.isEmpty() ? config.path() : error);
    } else {
        add("config", true, "not created yet; the daemon writes the defaults on first start", false);
    }

    {
        QProcess unit;
        unit.start("systemctl", {"--user", "is-active", "dialstudio.service"});
        unit.waitForFinished(2000);
        const QString state = QString::fromUtf8(unit.readAllStandardOutput()).trimmed();
        add("systemd user unit", state == "active", state.isEmpty() ? "not installed" : state, false);
    }
    IpcClient client;
    add("daemon socket", client.connectTo(Ipc::socketPath(), 300), Ipc::socketPath(), false);
    return checks;
}

int cmdDoctor(const Args &a) {
    const QList<Check> checks = runChecks();
    bool allRequired = true;
    QJsonArray json;
    for (const auto &c : checks) {
        allRequired = allRequired && (c.ok || !c.required);
        json.append(QJsonObject{{"name", c.name}, {"ok", c.ok}, {"required", c.required}, {"detail", c.detail}});
    }
    if (a.flags.contains("json")) {
        out << QJsonDocument(QJsonObject{{"ok", allRequired}, {"checks", json}}).toJson(QJsonDocument::Indented);
    } else {
        for (const auto &c : checks)
            out << (c.ok ? "ok    " : c.required ? "FAIL  " : "warn  ") << c.name.leftJustified(30) << c.detail << "\n";
    }
    return allRequired ? 0 : 1;
}

// ---------------------------------------------------------------------------

int cmdDaemon(const Args &) {
    Daemon::Options options;
    options.configPath = Config::defaultPath();
    options.socketPath = Ipc::socketPath();
    options.hyprConfig = OmarchyScan::defaultConfig();
    options.omarchyPath = OmarchyScan::defaultOmarchyPath();
    Daemon daemon(QDBusConnection::systemBus(), options);
    QString error;
    if (!daemon.start(&error)) return fail(error);
    daemon.installSignalHandlers();
    QObject::connect(&daemon, &Daemon::finished, QCoreApplication::instance(), &QCoreApplication::quit);
    qInfo().noquote() << "dialstudio daemon" << DIALSTUDIO_VERSION << "listening on" << options.socketPath
                      << "config" << options.configPath;
    return QCoreApplication::exec();
}

} // namespace

int Cli::run(const QStringList &argv) {
    if (argv.isEmpty() || argv[0] == "--help" || argv[0] == "-h" || argv[0] == "help") {
        out << QString(Usage).arg(DIALSTUDIO_VERSION);
        return argv.isEmpty() ? 1 : 0;
    }
    if (argv[0] == "--version" || argv[0] == "version") {
        out << "dialstudio " << DIALSTUDIO_VERSION << "\n";
        return 0;
    }
    const QString command = argv[0];
    const Args a = parse(argv.mid(1), {"on", "profile", "page", "via", "omarchy", "action"});
    if (a.bad) return fail(a.problem);

    if (command == "daemon") return cmdDaemon(a);
    if (command == "status") return cmdStatus(a);
    if (command == "events") return cmdEvents(a);
    if (command == "set") return cmdSet(a);
    if (command == "profile") return cmdProfile(a);
    if (command == "page") return cmdPage(a);
    if (command == "doctor") return cmdDoctor(a);
    if (command == "bind") return cmdBind(a);
    if (command == "unbind") return cmdUnbind(a);
    if (command == "bindings") return cmdBindings(a);
    if (command == "send") return cmdSend(a);
    if (command == "shortcuts") return cmdShortcuts(a);
    if (command == "learn") return cmdLearn(a);
    if (command == "open") return fail("the GUI arrives in v0.1.5; for now use the CLI (dialstudio --help)");
    errs << "dialstudio: unknown command '" << command << "'\n\n" << QString(Usage).arg(DIALSTUDIO_VERSION);
    return 2;
}
