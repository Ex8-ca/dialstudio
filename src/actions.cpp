#include "actions.h"

#include "shortcut.h"

#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>
#include <cmath>

namespace {

const QStringList InternalTypes = {"page", "profile", "haptics", "multi", "noop", "hypr_zoom"};
const QStringList AllTypes = {"volume", "mic", "brightness", "media", "keys", "text", "hypr", "omarchy",
                              "exec", "url", "shell", "page", "profile", "haptics", "multi", "noop", "hypr_zoom"};

QString str(const QJsonObject &o, const char *key) { return o.value(key).toString(); }

bool fail(QString *error, const QString &message) {
    if (error) *error = message;
    return false;
}

QString mediaKeysym(const QString &action) {
    if (action == "play-pause") return "XF86AudioPlay";
    if (action == "next") return "XF86AudioNext";
    if (action == "previous") return "XF86AudioPrev";
    if (action == "stop") return "XF86AudioStop";
    return {};
}

} // namespace

Tools Tools::detect(bool probeScope) {
    Tools tools;
    for (const char *name : {"wpctl", "wtype", "hyprctl", "brightnessctl", "notify-send", "playerctl", "swayosd-client",
                             "uwsm-app", "systemd-run", "gtk-launch", "xdg-open", "sh"})
        if (!QStandardPaths::findExecutable(name).isEmpty())
            tools.present.insert(name);
    if (probeScope && tools.has("systemd-run")) {
        // A scope keeps launched apps alive across daemon restarts; it only works with a user manager.
        QProcess probe;
        probe.start("systemd-run", {"--user", "--scope", "--collect", "--quiet", "--", "true"});
        tools.scope = probe.waitForFinished(3000) && probe.exitCode() == 0;
    }
    return tools;
}

bool Actions::isInternal(const QString &type) { return InternalTypes.contains(type); }

QString Actions::luaString(const QString &text) {
    QString out = "\"";
    for (const QChar c : text) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out + "\"";
}

Command Actions::zoomCommand(double factor) {
    return {"hyprctl", {"eval", QStringLiteral("hl.config({ cursor = { zoom_factor = %1 } })").arg(factor, 0, 'f', 2)}, false};
}

bool Actions::hyprLua(const QString &dispatch, const QString &arg, QString *lua, QString *error) {
    const QString a = luaString(arg);
    QString expr;
    if (dispatch == "workspace") expr = QStringLiteral("hl.dsp.focus({ workspace = %1 })").arg(a);
    else if (dispatch == "movetoworkspace") expr = QStringLiteral("hl.dsp.window.move({ workspace = %1 })").arg(a);
    else if (dispatch == "movefocus") expr = QStringLiteral("hl.dsp.focus({ direction = %1 })").arg(a);
    else if (dispatch == "movewindow") expr = QStringLiteral("hl.dsp.window.move({ direction = %1 })").arg(a);
    else if (dispatch == "swapwindow") expr = QStringLiteral("hl.dsp.window.swap({ direction = %1 })").arg(a);
    else if (dispatch == "togglefloating") expr = "hl.dsp.window.float({ action = \"toggle\" })";
    else if (dispatch == "fullscreen") expr = "hl.dsp.window.fullscreen({})";
    else if (dispatch == "killactive") expr = "hl.dsp.window.close({})";
    else if (dispatch == "cyclenext") expr = "hl.dsp.window.cycle_next({})";
    else if (dispatch == "togglespecialworkspace") expr = QStringLiteral("hl.dsp.workspace.toggle_special(%1)").arg(a);
    else if (dispatch == "exec") expr = QStringLiteral("hl.dsp.exec_cmd(%1)").arg(a);
    else return fail(error, QStringLiteral("unknown Hyprland dispatcher '%1'").arg(dispatch));

    const bool needsArg = dispatch == "workspace" || dispatch == "movetoworkspace" || dispatch == "movefocus" ||
                          dispatch == "movewindow" || dispatch == "swapwindow" || dispatch == "exec" ||
                          dispatch == "togglespecialworkspace";
    if (needsArg && arg.isEmpty())
        return fail(error, QStringLiteral("Hyprland dispatcher '%1' needs an arg").arg(dispatch));
    if (lua) *lua = expr;
    return true;
}

bool Actions::validate(const QJsonObject &a, QString *error) {
    const QString type = str(a, "type");
    if (!AllTypes.contains(type))
        return fail(error, type.isEmpty() ? QStringLiteral("action has no type")
                                          : QStringLiteral("unknown action type '%1'").arg(type));
    const QString action = str(a, "action");
    auto oneOf = [&](const QStringList &allowed) {
        return allowed.contains(action) ||
               fail(error, QStringLiteral("%1 action must be one of: %2").arg(type, allowed.join(", ")));
    };
    if (type == "volume") return oneOf({"up", "down", "mute"});
    if (type == "mic") return oneOf({"mute"});
    if (type == "brightness") return oneOf({"up", "down"});
    if (type == "media") return oneOf({"play-pause", "next", "previous", "stop"});
    if (type == "hypr_zoom") return oneOf({"in", "out", "reset"});
    if (type == "keys") {
        if (a.contains("omarchy")) {
            if (str(a, "omarchy").isEmpty()) return fail(error, "omarchy link needs a description");
            Combo cached;
            if (!str(a, "combo").isEmpty() && !Shortcut::parse(str(a, "combo"), &cached, error)) return false;
        } else {
            Combo combo;
            QString message;
            if (!Shortcut::parse(str(a, "combo"), &combo, &message)) return fail(error, message);
        }
        const QString via = str(a, "via");
        if (!via.isEmpty() && via != "wtype" && via != "hypr")
            return fail(error, "via must be wtype or hypr");
        return true;
    }
    if (type == "text") return !str(a, "text").isEmpty() || fail(error, "text action needs text");
    if (type == "hypr") {
        if (!str(a, "lua").isEmpty()) return true;
        return hyprLua(str(a, "dispatch"), str(a, "arg"), nullptr, error);
    }
    if (type == "omarchy") {
        static const QStringList known = {"menu", "screenshot", "screenrecord", "background-next", "lock", "run"};
        if (!known.contains(action)) return fail(error, "omarchy action must be one of: " + known.join(", "));
        if (action == "run" && !QRegularExpression("^omarchy-[A-Za-z0-9-]+$").match(str(a, "command")).hasMatch())
            return fail(error, "omarchy run needs a command named omarchy-...");
        return true;
    }
    if (type == "exec")
        return !str(a, "command").trimmed().isEmpty() || !str(a, "app").isEmpty() ||
               fail(error, "exec action needs command or app");
    if (type == "url") return !str(a, "url").isEmpty() || fail(error, "url action needs url");
    if (type == "shell") return !str(a, "command").isEmpty() || fail(error, "shell action needs command");
    if (type == "page") {
        if (action == "next" || action == "prev" || (action == "goto" && a.value("page").isDouble())) return true;
        return fail(error, "page action must be next, prev or goto with a page number");
    }
    if (type == "profile") {
        if (action == "auto" || (action == "goto" && !str(a, "name").isEmpty())) return true;
        return fail(error, "profile action must be auto or goto with a name");
    }
    if (type == "haptics") return oneOf({"toggle", "on", "off"});
    if (type == "multi") {
        const auto list = a.value("actions").toArray();
        if (list.isEmpty()) return fail(error, "multi action needs a list of actions");
        for (const auto &item : list)
            if (!validate(item.toObject(), error)) return false;
    }
    return true;
}

QList<Command> Actions::build(const QJsonObject &a, int steps, QString *error) const {
    const QString type = str(a, "type");
    const QString action = str(a, "action");
    steps = qBound(1, steps, 20);
    QList<Command> out;
    QString problem;
    if (!validate(a, &problem)) {
        if (error) *error = problem;
        return out;
    }
    if (isInternal(type))
        return out;

    auto require = [&](const QString &tool) {
        if (m_tools.has(tool)) return true;
        if (error) *error = QStringLiteral("%1 is not installed").arg(tool);
        return false;
    };
    auto repeat = [&](const Command &c) {
        for (int i = 0; i < steps; ++i) out << c;
    };

    if (type == "volume") {
        const int step = a.value("step").toInt(2);
        if (action == "mute") {
            if (m_tools.has("swayosd-client")) out << Command{"swayosd-client", {"--output-volume", "mute-toggle"}, false};
            else if (require("wpctl")) out << Command{"wpctl", {"set-mute", "@DEFAULT_AUDIO_SINK@", "toggle"}, false};
        } else if (m_tools.has("swayosd-client")) {
            out << Command{"swayosd-client", {"--output-volume", action == "up" ? "raise" : "lower", QString::number(step * steps)}, false};
        } else if (require("wpctl")) {
            out << Command{"wpctl", {"set-volume", "-l", "1.5", "@DEFAULT_AUDIO_SINK@",
                                     QString::number(step * steps) + (action == "up" ? "%+" : "%-")}, false};
        }
    } else if (type == "mic") {
        if (require("wpctl")) out << Command{"wpctl", {"set-mute", "@DEFAULT_AUDIO_SOURCE@", "toggle"}, false};
    } else if (type == "brightness") {
        const int step = a.value("step").toInt(5);
        if (m_tools.has("swayosd-client")) {
            repeat({"swayosd-client", {"--brightness", action == "up" ? "raise" : "lower"}, false});
        } else if (require("brightnessctl")) {
            out << Command{"brightnessctl", {"set", QString::number(step * steps) + (action == "up" ? "%+" : "%-")}, false};
        }
    } else if (type == "media") {
        if (m_tools.has("playerctl")) out << Command{"playerctl", {action}, false};
        else if (require("wtype")) out << Command{"wtype", {"-k", mediaKeysym(action)}, false};
    } else if (type == "keys") {
        QString comboText = str(a, "combo");
        if (a.contains("omarchy")) {
            comboText = m_resolver ? m_resolver(a) : QString();
            // The combo cached at bind time is used when the description can no longer be resolved.
            if (comboText.isEmpty()) comboText = str(a, "combo");
            if (comboText.isEmpty()) {
                if (error) *error = QStringLiteral("Omarchy shortcut '%1' not found").arg(str(a, "omarchy"));
                return out;
            }
        }
        Combo combo;
        if (!Shortcut::parse(comboText, &combo, error)) return out;
        // Omarchy links are delivered through Hyprland so its own binds see them.
        const QString via = str(a, "via").isEmpty() ? (a.contains("omarchy") ? "hypr" : "wtype") : str(a, "via");
        if (via == "hypr") {
            if (!require("hyprctl")) return out;
            QString window = str(a, "window");
            QString lua = QStringLiteral("hl.dsp.send_shortcut({ mods = %1, key = %2").arg(luaString(combo.hyprMods()), luaString(combo.key));
            if (!window.isEmpty()) lua += QStringLiteral(", window = %1").arg(luaString(window));
            lua += " })";
            repeat({"hyprctl", {"dispatch", lua}, false});
        } else if (require("wtype")) {
            repeat({"wtype", combo.wtypeArgs(), false});
        }
    } else if (type == "text") {
        if (require("wtype")) out << Command{"wtype", {"--", str(a, "text")}, false};
    } else if (type == "hypr") {
        QString lua = str(a, "lua");
        if (lua.isEmpty()) hyprLua(str(a, "dispatch"), str(a, "arg"), &lua, error);
        if (require("hyprctl")) repeat({"hyprctl", {"dispatch", lua}, false});
    } else if (type == "omarchy") {
        QString program;
        QStringList args;
        if (action == "menu") program = "omarchy-menu";
        else if (action == "screenshot") program = "omarchy-capture-screenshot";
        else if (action == "screenrecord") program = "omarchy-capture-screenrecording";
        else if (action == "background-next") program = "omarchy-theme-bg-next";
        else if (action == "lock") program = "omarchy-system-lock";
        else { program = str(a, "command"); for (const auto &v : a.value("args").toArray()) args << v.toString(); }
        out << Command{program, args, true};
    } else if (type == "exec") {
        QStringList argv;
        if (!str(a, "app").isEmpty()) {
            if (m_tools.has("uwsm-app")) argv = {"uwsm-app", "--", str(a, "app")};
            else if (m_tools.has("gtk-launch")) argv = {"gtk-launch", str(a, "app")};
            else { if (error) *error = "neither uwsm-app nor gtk-launch is installed"; return out; }
        } else {
            argv = QProcess::splitCommand(str(a, "command"));
            if (argv.isEmpty()) { if (error) *error = "empty command"; return out; }
            if (m_tools.has("uwsm-app")) argv = QStringList{"uwsm-app", "--"} + argv;
        }
        if (m_tools.scope) argv = QStringList{"systemd-run", "--user", "--scope", "--collect", "--quiet", "--"} + argv;
        out << Command{argv.first(), argv.mid(1), true};
    } else if (type == "url") {
        if (require("xdg-open")) out << Command{"xdg-open", {str(a, "url")}, true};
    } else if (type == "shell") {
        QStringList argv = {"sh", "-c", str(a, "command")};
        if (m_tools.scope) argv = QStringList{"systemd-run", "--user", "--scope", "--collect", "--quiet", "--"} + argv;
        out << Command{argv.first(), argv.mid(1), true};
    }
    return out;
}

void Actions::start(const Command &command, const QString &label) {
    if (m_dryRun) { emit ran(QStringList{command.program} + command.args); return; }
    if (command.detached) {
        if (!QProcess::startDetached(command.program, command.args))
            emit failed(label, QStringLiteral("could not start %1").arg(command.program));
        return;
    }
    auto *process = new QProcess(this);
    auto *watchdog = new QTimer(process);
    watchdog->setSingleShot(true);
    watchdog->setInterval(5000);
    connect(watchdog, &QTimer::timeout, process, [process, label, this] {
        process->kill();
        emit failed(label, "timed out after 5 s");
    });
    connect(process, &QProcess::errorOccurred, this, [=](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            emit failed(label, QStringLiteral("could not start %1").arg(command.program));
            process->deleteLater();
        }
    });
    connect(process, &QProcess::finished, this, [=](int code, QProcess::ExitStatus status) {
        if (status == QProcess::NormalExit && code != 0)
            emit failed(label, QStringLiteral("%1 exited with %2: %3").arg(command.program).arg(code)
                                   .arg(QString::fromUtf8(process->readAllStandardError()).trimmed()));
        process->deleteLater();
    });
    process->start(command.program, command.args);
    watchdog->start();
}

void Actions::runZoom(const QJsonObject &a, int steps) {
    if (!m_tools.has("hyprctl")) { emit failed("hypr_zoom", "hyprctl is not installed"); return; }
    const QString action = str(a, "action");
    if (m_dryRun) { emit ran({"hyprctl", "zoom", action}); return; }
    const double step = a.value("step").toDouble(0.1) * steps;
    auto *query = new QProcess(this);
    connect(query, &QProcess::finished, this, [=] {
        const double current = QJsonDocument::fromJson(query->readAllStandardOutput()).object().value("float").toDouble(1.0);
        double target = action == "reset" ? 1.0 : current + (action == "in" ? step : -step);
        target = std::round(qBound(1.0, target, 5.0) * 100) / 100;
        start(zoomCommand(target), "hypr_zoom");
        query->deleteLater();
    });
    query->start("hyprctl", {"-j", "getoption", "cursor:zoom_factor"});
}

void Actions::runMulti(const QJsonObject &a, int index) {
    const auto list = a.value("actions").toArray();
    if (index >= list.size()) return;
    const QJsonObject item = list[index].toObject();
    run(item, 1);
    const int delay = item.value("delay_ms").toInt(a.value("delay_ms").toInt(0));
    QTimer::singleShot(delay, this, [=] { runMulti(a, index + 1); });
}

void Actions::run(const QJsonObject &a, int steps) {
    const QString type = a.value("type").toString();
    QString error;
    if (type == "multi") { runMulti(a, 0); return; }
    if (type == "noop") return;
    if (type == "hypr_zoom") { runZoom(a, steps); return; }
    if (isInternal(type)) { emit internalAction(type, a); return; }
    const QList<Command> commands = build(a, steps, &error);
    if (!error.isEmpty()) { emit failed(type, error); return; }
    for (const auto &c : commands)
        start(c, type);
}
