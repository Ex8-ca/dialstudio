// Catalog of Omarchy's keyboard shortcuts, read from the Lua config that defines them.
//
// Hyprland 0.55+ reports every Lua bind as dispatcher "__lua" and injected keys never
// trigger binds, so a shortcut cannot be fired by key. Instead the source is evaluated
// (the same trick omarchy-menu-keybindings uses): the user's hyprland.lua is run in a
// plain Lua interpreter against a stub `hl`, and each hl.bind() call is recorded with its
// description, key, and what it does (an exec command, or an hl.dsp expression).
// Nothing is written: the config is only read.
#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QTimer>

struct OmarchyBind {
    QString description;
    int modmask = 0;          // Hyprland bits: shift=1, ctrl=4, alt=8, super=64
    QString key;              // as written in the config: RETURN, XF86AudioMute, code:10, comma
    QString kind;             // "exec", "lua", or "" for a Lua function Hyprland only reports as __lua
    QString arg;              // the command line, or the hl.dsp expression
    bool runnable = false;
    QString reason;           // why it is not runnable

    // "SUPER + SHIFT + RETURN"; code:N keys are shown as the key they usually are.
    QString combo() const;
    QString comboKey() const;     // canonical form, for comparing combos
    static QString canonicalCombo(const QString &text);
    QString summary() const;      // what running it does, for listings
    QJsonObject toJson() const;
};

struct OmarchyLookup {
    enum class Status { Found, NotFound, Ambiguous };
    Status status = Status::NotFound;
    OmarchyBind bind;
    QString message;
};

class OmarchyCatalog {
public:
    // Parses the scanner's tab-separated output: modmask, description, key, kind, arg.
    static QList<OmarchyBind> parse(const QByteArray &tsv);
    // Decides runnable/reason for one entry. Only commands and hl.dsp / o.workspaces
    // expressions are accepted, so a config cannot smuggle arbitrary Lua into hyprctl.
    static void classify(OmarchyBind *bind);

    void set(const QList<OmarchyBind> &binds) { m_binds = binds; m_loaded = true; }
    void clear() { m_binds.clear(); m_loaded = false; }
    bool loaded() const { return m_loaded; }
    const QList<OmarchyBind> &binds() const { return m_binds; }
    int runnableCount() const;

    // Finds a shortcut by description (exact, then case-insensitive, then a unique
    // substring). `combo` picks between entries with the same description, and is
    // ignored when none matches it (the user may have remapped the key since).
    // Entries that run the same thing count as one.
    OmarchyLookup find(const QString &description, const QString &combo = {}) const;
    QJsonArray toJson() const;

private:
    QList<OmarchyBind> m_binds;
    bool m_loaded = false;
};

// Runs the Lua scanner. Asynchronous for the daemon (a stuck scan must not stall the
// heartbeat), with a blocking variant for the CLI.
class OmarchyScan : public QObject {
    Q_OBJECT
public:
    explicit OmarchyScan(QObject *parent = nullptr);
    ~OmarchyScan() override;

    static QString defaultConfig();       // ~/.config/hypr/hyprland.lua
    static QString defaultOmarchyPath();  // $OMARCHY_PATH or /usr/share/omarchy
    static QByteArray script();

    void start(const QString &hyprConfig, const QString &omarchyPath);
    bool running() const { return m_process.state() != QProcess::NotRunning; }

    // Blocking variant (CLI, tests).
    static bool run(const QString &hyprConfig, const QString &omarchyPath, QList<OmarchyBind> *out,
                    QString *error, int timeoutMs = 5000);

signals:
    void finished(bool ok, const QList<OmarchyBind> &binds, const QString &error);

private:
    static void configure(QProcess *process, const QString &hyprConfig, const QString &omarchyPath);
    static bool interpret(QProcess *process, QList<OmarchyBind> *out, QString *error);

    QProcess m_process;
    QTimer m_timeout;
};

Q_DECLARE_METATYPE(QList<OmarchyBind>)
