// Action registry: validates action objects, builds the argv to run (pure, so
// tests can assert it), and runs them without ever blocking the event loop.
#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <functional>

#include "omarchybinds.h"

struct Tools {
    QSet<QString> present;
    bool scope = false;     // systemd-run --user --scope works here
    bool has(const QString &name) const { return present.contains(name); }
    static Tools detect(bool probeScope);
};

struct Command {
    QString program;
    QStringList args;
    bool detached = false;  // launched apps: must outlive the daemon, no watchdog

    bool operator==(const Command &o) const { return program == o.program && args == o.args && detached == o.detached; }
};

class Actions : public QObject {
    Q_OBJECT
public:
    explicit Actions(const Tools &tools, QObject *parent = nullptr) : QObject(parent), m_tools(tools) {}

    // Types handled by the daemon itself rather than by spawning a process.
    static bool isInternal(const QString &type);
    // Fills *error with a human message; used by config validation and `bind`.
    static bool validate(const QJsonObject &action, QString *error);
    // Lua expression for a legacy dispatcher name + argument (Hyprland 0.55+ is Lua only).
    static bool hyprLua(const QString &dispatch, const QString &arg, QString *lua, QString *error);
    static QString luaString(const QString &text);
    static Command zoomCommand(double factor);

    // Looks an {"type":"omarchy_bind","description":...} link up in the Omarchy shortcut
    // catalog. The bind is then run (its command, or its Hyprland dispatch), never typed.
    using OmarchyResolver = std::function<OmarchyLookup(const QString &description, const QString &combo, bool loose)>;
    void setOmarchyResolver(OmarchyResolver resolver) { m_resolver = std::move(resolver); }
    // The argv that runs one Omarchy bind, or empty with *error set when it cannot run.
    QList<Command> buildOmarchyBind(const OmarchyBind &bind, int steps, QString *error) const;
    void setTools(const Tools &tools) { m_tools = tools; }
    // Tests: report what would run through ran() instead of spawning anything.
    void setDryRun(bool on) { m_dryRun = on; }

    // Argv for an external action. `steps` repeats or scales (volume, brightness).
    // Internal actions return an empty list with *error unset.
    QList<Command> build(const QJsonObject &action, int steps, QString *error) const;

    void run(const QJsonObject &action, int steps = 1);

signals:
    void failed(const QString &what, const QString &message);
    void internalAction(const QString &type, const QJsonObject &action);
    void ran(const QStringList &argv);   // only emitted in dry-run mode

private:
    void start(const Command &command, const QString &label);
    void runZoom(const QJsonObject &action, int steps);
    void runMulti(const QJsonObject &action, int index);

    Tools m_tools;
    bool m_dryRun = false;
    OmarchyResolver m_resolver;
};
