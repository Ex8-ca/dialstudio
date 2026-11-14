#include "testmain.h"

#include <QFile>
#include <QTemporaryDir>

#include "actions.h"
#include "omarchybinds.h"

#ifndef FIXTURE_DIR
#define FIXTURE_DIR "fixtures"
#endif

// The Omarchy shortcut catalog: the Lua scanner against fixture configs (never the user's),
// the runnable/not-runnable classification, lookup by description, and the argv a bind runs.
class OmarchyBindsTests : public QObject {
    Q_OBJECT

    static QString fixture(const char *name) { return QStringLiteral(FIXTURE_DIR "/omarchy/") + name; }

    static QList<OmarchyBind> scan(const char *name, QString *error = nullptr) {
        QList<OmarchyBind> binds;
        QString e;
        OmarchyScan::run(fixture(name), "/nonexistent-omarchy", &binds, &e, 5000);
        if (error) *error = e;
        return binds;
    }
    static OmarchyBind find(const QList<OmarchyBind> &binds, const QString &description) {
        for (const auto &b : binds) if (b.description == description) return b;
        return {};
    }
    static Tools tools(std::initializer_list<const char *> names) {
        Tools t;
        for (auto n : names) t.present.insert(n);
        return t;
    }

private slots:
    void scannerReadsCommandsAndDispatchers() {
        const auto binds = scan("hyprland.lua");
        QVERIFY(!binds.isEmpty());
        const OmarchyBind terminal = find(binds, "Terminal");
        QCOMPARE(terminal.kind, QString("exec"));
        QCOMPARE(terminal.arg, QString("omarchy-launch-terminal"));
        QCOMPARE(terminal.combo(), QString("SUPER + RETURN"));
        QVERIFY(terminal.runnable);

        const OmarchyBind volume = find(binds, "Volume up");
        QCOMPARE(volume.kind, QString("lua"));
        QCOMPARE(volume.arg, QString("hl.dsp.global(\"omarchy:audio.raise\")"));
        QCOMPARE(volume.modmask, 0);
        QVERIFY(volume.runnable);

        QCOMPARE(find(binds, "Toggle window floating/tiling").arg, QString("hl.dsp.window.float({ action = \"toggle\" })"));
        QCOMPARE(find(binds, "All modifiers").combo(), QString("SUPER + SHIFT + CTRL + ALT + comma"));
        QCOMPARE(find(binds, "All modifiers").modmask, 64 + 1 + 4 + 8);
    }

    void scannerExpandsLoopsAndSkipsUndescribedBinds() {
        const auto binds = scan("hyprland.lua");
        QCOMPARE(find(binds, "Switch to workspace 2").key, QString("code:11"));
        QCOMPARE(find(binds, "Switch to workspace 3").arg, QString("hl.dsp.focus({ workspace = \"3\" })"));
        for (const auto &b : binds) QVERIFY2(b.arg != "no-description-is-skipped", "a bind without a description must be skipped");
    }

    void unbindDropsTheDefaultSoOverridesWin() {
        const auto binds = scan("hyprland.lua");
        int fileManagers = 0;
        for (const auto &b : binds) fileManagers += b.description == "File manager";
        QCOMPARE(fileManagers, 1);
        QCOMPARE(find(binds, "File manager").arg, QString("uwsm-app -- /home/u/.local/bin/strata"));
    }

    void luaFunctionsAreListedButNotRunnable() {
        const OmarchyBind copy = find(scan("hyprland.lua"), "Universal copy");
        QCOMPARE(copy.description, QString("Universal copy"));
        QVERIFY(!copy.runnable);
        QVERIFY(copy.reason.contains("Lua function"));
        QVERIFY(copy.toJson().contains("reason"));
    }

    void tabsInDescriptionsSurvive() {
        QCOMPARE(find(scan("hyprland.lua"), "Tab\tin name").arg, QString("omarchy-tab"));
    }

    void classifyRefusesArbitraryLua() {
        OmarchyBind b;
        b.kind = "lua";
        b.arg = "os.execute('rm -rf ~')";
        OmarchyCatalog::classify(&b);
        QVERIFY(!b.runnable);
        b.arg = "hl.dsp.exec_cmd(\"x\")";
        OmarchyCatalog::classify(&b);
        QVERIFY(b.runnable);
        b.arg = "o.workspaces.focus(3)";
        OmarchyCatalog::classify(&b);
        QVERIFY(b.runnable);
        b.kind = "exec";
        b.arg = "   ";
        OmarchyCatalog::classify(&b);
        QVERIFY(!b.runnable);
    }

    void parseHandlesTsvAndBadLines() {
        const auto binds = OmarchyCatalog::parse("64\tTerminal\tRETURN\texec\tfoot\n"
                                                 "garbage line\n"
                                                 "x\tbad mask\tA\texec\tfoo\n"
                                                 "4\tQuit\tQ\tlua\thl.dsp.window.close()\n"
                                                 "0\tFn\tF1\t\t\n");
        QCOMPARE(binds.size(), 3);
        QCOMPARE(binds[0].combo(), QString("SUPER + RETURN"));
        QVERIFY(binds[1].runnable);
        QVERIFY(!binds[2].runnable);
    }

    void brokenConfigStillYieldsEarlierBinds() {
        QString error;
        const auto binds = scan("broken.lua", &error);
        QCOMPARE(binds.size(), 1);
        QCOMPARE(binds[0].description, QString("Before the error"));
        QVERIFY2(error.contains("blew up"), qPrintable(error));
    }

    void runawayConfigIsStopped() {
        QList<OmarchyBind> binds;
        QString error;
        QElapsedTimer timer;
        timer.start();
        QVERIFY(!OmarchyScan::run(fixture("loop.lua"), "/x", &binds, &error, 8000));
        QVERIFY2(timer.elapsed() < 8000, "the instruction limit should stop the loop long before the timeout");
        QVERIFY(binds.isEmpty());
    }

    void missingConfigIsAnError() {
        QList<OmarchyBind> binds;
        QString error;
        QVERIFY(!OmarchyScan::run("/nonexistent/hyprland.lua", "/x", &binds, &error));
        QVERIFY(!error.isEmpty());
    }

    void lookupByDescription() {
        OmarchyCatalog c;
        c.set(scan("hyprland.lua"));
        QCOMPARE(c.find("Terminal").status, OmarchyLookup::Status::Found);
        QCOMPARE(c.find("terminal").bind.arg, QString("omarchy-launch-terminal"));      // case-insensitive
        QCOMPARE(c.find("Volume").status, OmarchyLookup::Status::Found);                // unique substring
        QCOMPARE(c.find("Nope entirely").status, OmarchyLookup::Status::NotFound);
        QCOMPARE(c.find("Switch to workspace").status, OmarchyLookup::Status::Ambiguous);
        QVERIFY(c.find("Switch to workspace").message.contains("several"));
        QCOMPARE(c.find("").status, OmarchyLookup::Status::NotFound);
    }

    void sameActionOnTwoChordsIsOneShortcut() {
        OmarchyCatalog c;
        c.set(scan("hyprland.lua"));
        const OmarchyLookup close = c.find("Close window");
        QCOMPARE(close.status, OmarchyLookup::Status::Found);
        QVERIFY(close.bind.runnable);
    }

    void sameDescriptionForDifferentActionsNeedsTheCombo() {
        OmarchyCatalog c;
        c.set(scan("hyprland.lua"));
        QCOMPARE(c.find("Reveal active window on top").status, OmarchyLookup::Status::Ambiguous);
        const OmarchyLookup picked = c.find("Reveal active window on top", "alt+shift+tab");
        QCOMPARE(picked.status, OmarchyLookup::Status::Found);
        QVERIFY(picked.bind.arg.contains("alter_zorder"));
        // A remembered combo that no longer exists falls back to the description alone.
        QCOMPARE(c.find("Terminal", "ctrl+x").bind.arg, QString("omarchy-launch-terminal"));
    }

    void removedShortcutIsNotFoundAfterRescan() {
        OmarchyCatalog c;
        c.set(scan("hyprland.lua"));
        QCOMPARE(c.find("Tmux").status, OmarchyLookup::Status::Found);
        QList<OmarchyBind> fewer;
        for (const auto &b : c.binds()) if (b.description != "Tmux") fewer << b;
        c.set(fewer);
        QCOMPARE(c.find("Tmux").status, OmarchyLookup::Status::NotFound);
    }

    void execBindRunsItsCommandDirectly() {
        Actions a(tools({"hyprctl", "sh"}));
        QString e;
        OmarchyBind b = find(scan("hyprland.lua"), "Tmux");
        const auto cmds = a.buildOmarchyBind(b, 1, &e);
        QVERIFY2(!cmds.isEmpty(), qPrintable(e));
        QCOMPARE(cmds[0].program, QString("omarchy-launch-terminal-tmux"));
        QCOMPARE(cmds[0].args, (QStringList{"--flag", "value"}));
        QVERIFY(cmds[0].detached);
    }

    void shellSyntaxGoesThroughHyprlandExec() {
        Actions a(tools({"hyprctl", "sh"}));
        QString e;
        const OmarchyBind b = find(scan("hyprland.lua"), "Pipes and quoting");
        const auto cmds = a.buildOmarchyBind(b, 1, &e);
        QVERIFY2(!cmds.isEmpty(), qPrintable(e));
        QCOMPARE(cmds[0].program, QString("hyprctl"));
        QVERIFY(cmds[0].args[1].startsWith("hl.dsp.exec_cmd(\""));
        // Without hyprctl it falls back to sh -c.
        Actions bare(tools({"sh"}));
        const auto fallback = bare.buildOmarchyBind(b, 1, &e);
        QCOMPARE(fallback[0].program, QString("sh"));
    }

    void dispatcherBindRunsThroughHyprctlDispatch() {
        Actions a(tools({"hyprctl"}));
        QString e;
        const auto cmds = a.buildOmarchyBind(find(scan("hyprland.lua"), "Volume up"), 1, &e);
        QCOMPARE(cmds[0].program, QString("hyprctl"));
        QCOMPARE(cmds[0].args, (QStringList{"dispatch", "hl.dsp.global(\"omarchy:audio.raise\")"}));
        Actions without(tools({}));
        QVERIFY(without.buildOmarchyBind(find(scan("hyprland.lua"), "Volume up"), 1, &e).isEmpty());
        QVERIFY(e.contains("hyprctl"));
    }

    void nonRunnableBindRefusesToRun() {
        Actions a(tools({"hyprctl"}));
        QString e;
        QVERIFY(a.buildOmarchyBind(find(scan("hyprland.lua"), "Universal copy"), 1, &e).isEmpty());
        QVERIFY(e.contains("cannot be run"));
    }

    void omarchyBindActionResolvesThroughTheCatalog() {
        OmarchyCatalog c;
        c.set(scan("hyprland.lua"));
        Actions a(tools({"hyprctl"}));
        a.setOmarchyResolver([&c](const QString &d, const QString &combo) { return c.find(d, combo); });
        QString e;
        const QJsonObject link{{"type", "omarchy_bind"}, {"description", "Terminal"}, {"combo", "SUPER + RETURN"}};
        QVERIFY(Actions::validate(link, &e));
        const auto cmds = a.build(link, 1, &e);
        QVERIFY2(!cmds.isEmpty(), qPrintable(e));
        QCOMPARE(cmds[0].program, QString("omarchy-launch-terminal"));
        // The v0.1.0 spelling still loads and now runs the command instead of typing keys.
        const auto legacy = a.build(QJsonObject{{"type", "keys"}, {"omarchy", "Terminal"}, {"combo", "super+Return"}, {"via", "hypr"}}, 1, &e);
        QCOMPARE(legacy[0].program, QString("omarchy-launch-terminal"));
        // A vanished description is an error that names it.
        QVERIFY(a.build(QJsonObject{{"type", "omarchy_bind"}, {"description", "Gone"}}, 1, &e).isEmpty());
        QVERIFY2(e.contains("Gone"), qPrintable(e));
        QVERIFY(!Actions::validate(QJsonObject{{"type", "omarchy_bind"}}, &e));
    }
};

REGISTER_TEST(OmarchyBindsTests)
#include "tests_omarchybinds.moc"
