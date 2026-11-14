#include "testmain.h"

#include <QSignalSpy>

#include "actions.h"

namespace {
Tools tools(std::initializer_list<const char *> names, bool scope = false) {
    Tools t;
    for (const char *n : names) t.present.insert(n);
    t.scope = scope;
    return t;
}
QJsonObject obj(QByteArray json) { return QJsonDocument::fromJson(json.replace('\'', '"')).object(); }
QStringList argv(const Command &c) { return QStringList{c.program} + c.args; }
}

class ActionTests : public QObject {
    Q_OBJECT
private slots:
    void volume() {
        Actions a(tools({"wpctl"}));
        QString e;
        auto up = a.build(obj("{'type':'volume','action':'up','step':2}"), 1, &e);
        QCOMPARE(argv(up[0]), (QStringList{"wpctl", "set-volume", "-l", "1.5", "@DEFAULT_AUDIO_SINK@", "2%+"}));
        auto down = a.build(obj("{'type':'volume','action':'down','step':2}"), 3, &e);
        QCOMPARE(down.size(), 1);
        QCOMPARE(argv(down[0]).last(), QString("6%-"));       // steps scale the percentage in one call
        auto mute = a.build(obj("{'type':'volume','action':'mute'}"), 1, &e);
        QCOMPARE(argv(mute[0]), (QStringList{"wpctl", "set-mute", "@DEFAULT_AUDIO_SINK@", "toggle"}));
    }
    void volumeUsesSwayosdWhenPresent() {
        Actions a(tools({"wpctl", "swayosd-client"}));
        QString e;
        auto up = a.build(obj("{'type':'volume','action':'up','step':2}"), 1, &e);
        QCOMPARE(argv(up[0]), (QStringList{"swayosd-client", "--output-volume", "raise", "2"}));
    }
    void missingToolIsReportedNotSkipped() {
        Actions a(tools({}));
        QString e;
        QVERIFY(a.build(obj("{'type':'volume','action':'up'}"), 1, &e).isEmpty());
        QCOMPARE(e, QString("wpctl is not installed"));
    }
    void micAndBrightness() {
        Actions a(tools({"wpctl", "brightnessctl"}));
        QString e;
        QCOMPARE(argv(a.build(obj("{'type':'mic','action':'mute'}"), 1, &e)[0]),
                 (QStringList{"wpctl", "set-mute", "@DEFAULT_AUDIO_SOURCE@", "toggle"}));
        QCOMPARE(argv(a.build(obj("{'type':'brightness','action':'up'}"), 1, &e)[0]),
                 (QStringList{"brightnessctl", "set", "5%+"}));
        QCOMPARE(argv(a.build(obj("{'type':'brightness','action':'down','step':10}"), 2, &e)[0]),
                 (QStringList{"brightnessctl", "set", "20%-"}));
    }
    void mediaPrefersPlayerctlElseWtype() {
        QString e;
        Actions withPlayerctl(tools({"playerctl", "wtype"}));
        QCOMPARE(argv(withPlayerctl.build(obj("{'type':'media','action':'next'}"), 1, &e)[0]), (QStringList{"playerctl", "next"}));
        Actions fallback(tools({"wtype"}));
        QCOMPARE(argv(fallback.build(obj("{'type':'media','action':'play-pause'}"), 1, &e)[0]), (QStringList{"wtype", "-k", "XF86AudioPlay"}));
        QCOMPARE(argv(fallback.build(obj("{'type':'media','action':'previous'}"), 1, &e)[0]), (QStringList{"wtype", "-k", "XF86AudioPrev"}));
    }
    void keysViaWtype() {
        Actions a(tools({"wtype"}));
        QString e;
        auto cmds = a.build(obj("{'type':'keys','combo':'ctrl+shift+z'}"), 1, &e);
        QCOMPARE(argv(cmds[0]), (QStringList{"wtype", "-M", "ctrl", "-M", "shift", "-k", "z", "-m", "shift", "-m", "ctrl"}));
    }
    void keysViaHyprSendShortcut() {
        Actions a(tools({"hyprctl"}));
        QString e;
        auto cmds = a.build(obj("{'type':'keys','combo':'ctrl+shift+z','via':'hypr'}"), 1, &e);
        QCOMPARE(argv(cmds[0]), (QStringList{"hyprctl", "dispatch", "hl.dsp.send_shortcut({ mods = \"CTRL SHIFT\", key = \"z\" })"}));
        auto targeted = a.build(obj("{'type':'keys','combo':'super+1','via':'hypr','window':'class:^firefox$'}"), 1, &e);
        QCOMPARE(argv(targeted[0]).last(), QString("hl.dsp.send_shortcut({ mods = \"SUPER\", key = \"1\", window = \"class:^firefox$\" })"));
    }
    void omarchyLinkResolvesByDescriptionAndFallsBackToCachedCombo() {
        Actions a(tools({"hyprctl"}));
        QString e;
        a.setShortcutResolver([](const QJsonObject &l) { return l.value("omarchy").toString() == "Volume up" ? QString("XF86AudioRaiseVolume") : QString(); });
        auto found = a.build(obj("{'type':'keys','omarchy':'Volume up','combo':'ctrl+x','via':'hypr'}"), 1, &e);
        QVERIFY2(!found.isEmpty(), qPrintable(e));
        QCOMPARE(argv(found[0]).last(), QString("hl.dsp.send_shortcut({ mods = \"\", key = \"XF86AudioRaiseVolume\" })"));
        auto cached = a.build(obj("{'type':'keys','omarchy':'Gone','combo':'ctrl+q','via':'hypr'}"), 1, &e);
        QVERIFY2(!cached.isEmpty(), qPrintable(e));
        QCOMPARE(argv(cached[0]).last(), QString("hl.dsp.send_shortcut({ mods = \"CTRL\", key = \"q\" })"));
        QVERIFY(a.build(obj("{'type':'keys','omarchy':'Gone','via':'hypr'}"), 1, &e).isEmpty());
        QVERIFY(e.contains("not found"));
    }
    void text() {
        Actions a(tools({"wtype"}));
        QString e;
        QCOMPARE(argv(a.build(obj("{'type':'text','text':'-hello'}"), 1, &e)[0]), (QStringList{"wtype", "--", "-hello"}));
    }
    void hyprDispatchersBecomeLua() {
        Actions a(tools({"hyprctl"}));
        QString e;
        QCOMPARE(argv(a.build(obj("{'type':'hypr','dispatch':'workspace','arg':'+1'}"), 1, &e)[0]),
                 (QStringList{"hyprctl", "dispatch", "hl.dsp.focus({ workspace = \"+1\" })"}));
        QCOMPARE(argv(a.build(obj("{'type':'hypr','dispatch':'movefocus','arg':'l'}"), 1, &e)[0]).last(),
                 QString("hl.dsp.focus({ direction = \"l\" })"));
        QCOMPARE(argv(a.build(obj("{'type':'hypr','dispatch':'killactive'}"), 1, &e)[0]).last(), QString("hl.dsp.window.close({})"));
        QCOMPARE(argv(a.build(obj("{'type':'hypr','dispatch':'togglefloating'}"), 1, &e)[0]).last(), QString("hl.dsp.window.float({ action = \"toggle\" })"));
        const QByteArray quoted = "{\"type\":\"hypr\",\"dispatch\":\"exec\",\"arg\":\"foot \\\"x\\\"\"}";
        QCOMPARE(argv(a.build(QJsonDocument::fromJson(quoted).object(), 1, &e)[0]).last(),
                 QString("hl.dsp.exec_cmd(\"foot \\\"x\\\"\")"));
        QCOMPARE(argv(a.build(obj("{'type':'hypr','lua':'hl.dsp.focus({ workspace = 3 })'}"), 1, &e)[0]).last(), QString("hl.dsp.focus({ workspace = 3 })"));
        QString err;
        QVERIFY(!Actions::validate(obj("{'type':'hypr','dispatch':'nonsense'}"), &err));
        QVERIFY(!Actions::validate(obj("{'type':'hypr','dispatch':'workspace'}"), &err));
    }
    void zoomCommandIsLuaConfig() {
        QCOMPARE(argv(Actions::zoomCommand(1.5)), (QStringList{"hyprctl", "eval", "hl.config({ cursor = { zoom_factor = 1.50 } })"}));
    }
    void execPrefersUwsmAndDetachesInScope() {
        QString e;
        Actions plain(tools({}));
        auto c0 = plain.build(obj("{'type':'exec','command':'foot -e htop'}"), 1, &e);
        QCOMPARE(argv(c0[0]), (QStringList{"foot", "-e", "htop"}));
        QVERIFY(c0[0].detached);
        Actions uwsm(tools({"uwsm-app"}));
        QCOMPARE(argv(uwsm.build(obj("{'type':'exec','command':'foot -e htop'}"), 1, &e)[0]), (QStringList{"uwsm-app", "--", "foot", "-e", "htop"}));
        QCOMPARE(argv(uwsm.build(obj("{'type':'exec','app':'org.gnome.Nautilus'}"), 1, &e)[0]), (QStringList{"uwsm-app", "--", "org.gnome.Nautilus"}));
        Actions scoped(tools({"uwsm-app", "systemd-run"}, true));
        QCOMPARE(argv(scoped.build(obj("{'type':'exec','command':'foot'}"), 1, &e)[0]),
                 (QStringList{"systemd-run", "--user", "--scope", "--collect", "--quiet", "--", "uwsm-app", "--", "foot"}));
    }
    void shellIsTheOnlyShellType() {
        Actions a(tools({}));
        QString e;
        QCOMPARE(argv(a.build(obj("{'type':'shell','command':'echo hi | cat'}"), 1, &e)[0]), (QStringList{"sh", "-c", "echo hi | cat"}));
        // `exec` must never interpret shell syntax: the pipe is a plain argument.
        QCOMPARE(argv(a.build(obj("{'type':'exec','command':'echo hi | cat'}"), 1, &e)[0]), (QStringList{"echo", "hi", "|", "cat"}));
    }
    void urlOmarchyAndInternal() {
        Actions a(tools({"xdg-open"}));
        QString e;
        QCOMPARE(argv(a.build(obj("{'type':'url','url':'https://example.com'}"), 1, &e)[0]), (QStringList{"xdg-open", "https://example.com"}));
        QCOMPARE(argv(a.build(obj("{'type':'omarchy','action':'screenshot'}"), 1, &e)[0]), (QStringList{"omarchy-capture-screenshot"}));
        QCOMPARE(argv(a.build(obj("{'type':'omarchy','action':'menu'}"), 1, &e)[0]), (QStringList{"omarchy-menu"}));
        QCOMPARE(argv(a.build(obj("{'type':'omarchy','action':'run','command':'omarchy-theme-bg-next'}"), 1, &e)[0]), (QStringList{"omarchy-theme-bg-next"}));
        QVERIFY(!Actions::validate(obj("{'type':'omarchy','action':'run','command':'rm'}"), &e));
        e.clear();
        QVERIFY(a.build(obj("{'type':'page','action':'next'}"), 1, &e).isEmpty());
        QVERIFY(e.isEmpty());
        QVERIFY(Actions::isInternal("page"));
        QVERIFY(!Actions::isInternal("volume"));
    }
    void validation_data() {
        QTest::addColumn<QString>("json");
        QTest::addColumn<bool>("valid");
        QTest::newRow("ok volume") << "{\"type\":\"volume\",\"action\":\"up\"}" << true;
        QTest::newRow("bad volume") << "{\"type\":\"volume\",\"action\":\"sideways\"}" << false;
        QTest::newRow("no type") << "{\"action\":\"up\"}" << false;
        QTest::newRow("page goto") << "{\"type\":\"page\",\"action\":\"goto\",\"page\":2}" << true;
        QTest::newRow("page goto no number") << "{\"type\":\"page\",\"action\":\"goto\"}" << false;
        QTest::newRow("profile") << "{\"type\":\"profile\",\"action\":\"goto\",\"name\":\"X\"}" << true;
        QTest::newRow("multi ok") << "{\"type\":\"multi\",\"actions\":[{\"type\":\"noop\"},{\"type\":\"mic\",\"action\":\"mute\"}]}" << true;
        QTest::newRow("multi bad child") << "{\"type\":\"multi\",\"actions\":[{\"type\":\"nope\"}]}" << false;
        QTest::newRow("exec empty") << "{\"type\":\"exec\",\"command\":\"  \"}" << false;
        QTest::newRow("keys bad via") << "{\"type\":\"keys\",\"combo\":\"a\",\"via\":\"xdotool\"}" << false;
        QTest::newRow("hypr_zoom") << "{\"type\":\"hypr_zoom\",\"action\":\"in\"}" << true;
    }
    void validation() {
        QFETCH(QString, json);
        QFETCH(bool, valid);
        QString e;
        QCOMPARE(Actions::validate(QJsonDocument::fromJson(json.toUtf8()).object(), &e), valid);
        QCOMPARE(e.isEmpty(), valid);
    }
    void runReportsFailuresAndTimesOutNever() {
        Actions a(tools({"wpctl"}));
        QSignalSpy failed(&a, &Actions::failed);
        a.run(obj("{'type':'brightness','action':'up'}"));       // brightnessctl missing here
        QCOMPARE(failed.count(), 1);
        QVERIFY(failed[0][1].toString().contains("brightnessctl"));
    }
    void runInternalEmitsSignal() {
        Actions a(tools({}));
        QSignalSpy internal(&a, &Actions::internalAction);
        a.run(obj("{'type':'page','action':'next'}"));
        QCOMPARE(internal.count(), 1);
        QCOMPARE(internal[0][0].toString(), QString("page"));
    }
    void zeroStepsNeverHappenAndStepsAreCapped() {
        Actions a(tools({"wpctl"}));
        QString e;
        QCOMPARE(argv(a.build(obj("{'type':'volume','action':'up','step':1}"), 0, &e)[0]).last(), QString("1%+"));
        QCOMPARE(argv(a.build(obj("{'type':'volume','action':'up','step':1}"), 500, &e)[0]).last(), QString("20%+"));
    }
};

REGISTER_TEST(ActionTests)
#include "tests_actions.moc"
