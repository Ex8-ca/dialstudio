#include "testmain.h"

#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "config.h"
#include "shortcut.h"

class ConfigTests : public QObject {
    Q_OBJECT

    static QByteArray read(const QString &path) { QFile f(path); f.open(QIODevice::ReadOnly); return f.readAll(); }
    static void write(const QString &path, const QByteArray &bytes) { QFile f(path); QVERIFY(f.open(QIODevice::WriteOnly)); f.write(bytes); }

private slots:
    void defaultsAreValidAndReproduceFactoryBehaviour() {
        QStringList errors;
        QVERIFY2(Config::validate(Config::defaults(), &errors), qPrintable(errors.join("; ")));
        const QJsonObject b = Config::defaults().value("profiles").toArray()[0].toObject()
                                  .value("pages").toArray()[0].toObject().value("bindings").toObject();
        QCOMPARE(b.value("key2").toObject().value("press").toObject().value("action").toString(), QString("play-pause"));
        QCOMPARE(b.value("key7").toObject().value("press").toObject().value("combo").toString(), QString("ctrl+shift+z"));
        QCOMPARE(b.value("knob").toObject().value("rotate_cw").toObject().value("type").toString(), QString("volume"));
    }

    void missingFileIsCreatedWithDefaults() {
        QTemporaryDir dir;
        Config c(dir.filePath("sub/config.json"));
        QString error;
        QVERIFY2(c.load(&error), qPrintable(error));
        QVERIFY(QFile::exists(dir.filePath("sub/config.json")));
        QCOMPARE(QJsonDocument::fromJson(read(dir.filePath("sub/config.json"))).object().value("version").toInt(), 1);
    }

    void existingFileIsNeverOverwrittenWhenBroken() {
        QTemporaryDir dir;
        const QString path = dir.filePath("config.json");
        write(path, "{ this is not json");
        Config c(path);
        QString error;
        QVERIFY(!c.load(&error));
        QVERIFY(error.contains("config.json"));
        QCOMPARE(read(path), QByteArray("{ this is not json"));
    }

    void validationNamesTheProblem_data() {
        QTest::addColumn<QString>("mutation");
        QTest::addColumn<QString>("expected");
        QTest::newRow("version") << "version" << "unsupported config version";
        QTest::newRow("slot") << "slot" << "has no 'tap' slot";
        QTest::newRow("action") << "action" << "unknown action type";
        QTest::newRow("combo") << "combo" << "unknown key";
        QTest::newRow("regex") << "regex" << "not a valid regular expression";
        QTest::newRow("fallback") << "fallback" << "fallback";
        QTest::newRow("dup") << "dup" << "used twice";
        QTest::newRow("sleep") << "sleep" << "sleep_seconds";
    }
    void validationNamesTheProblem() {
        QFETCH(QString, mutation);
        QFETCH(QString, expected);
        QJsonObject doc = Config::defaults();
        QJsonArray profiles = doc.value("profiles").toArray();
        QJsonObject p0 = profiles[0].toObject();
        QJsonArray pages = p0.value("pages").toArray();
        QJsonObject page = pages[0].toObject();
        QJsonObject bindings = page.value("bindings").toObject();
        if (mutation == "version") doc["version"] = 2;
        if (mutation == "slot") bindings["key1"] = QJsonObject{{"tap", QJsonObject{{"type", "noop"}}}};
        if (mutation == "action") bindings["key1"] = QJsonObject{{"press", QJsonObject{{"type", "explode"}}}};
        if (mutation == "combo") bindings["key1"] = QJsonObject{{"press", QJsonObject{{"type", "keys"}, {"combo", "ctrl+Retrun"}}}};
        if (mutation == "sleep") { QJsonObject d = doc["device"].toObject(); d["sleep_seconds"] = 99999; doc["device"] = d; }
        page["bindings"] = bindings;
        pages[0] = page;
        p0["pages"] = pages;
        if (mutation == "fallback") p0["match"] = QJsonObject{{"class", "x"}};
        profiles[0] = p0;
        if (mutation == "regex") profiles.append(QJsonObject{{"name", "B"}, {"match", QJsonObject{{"class", "("}}}, {"pages", pages}});
        if (mutation == "dup") profiles.append(QJsonObject{{"name", "Default"}, {"match", QJsonObject{{"class", "x"}}}, {"pages", pages}});
        doc["profiles"] = profiles;
        QStringList errors;
        QVERIFY(!Config::validate(doc, &errors));
        QVERIFY2(errors.join("; ").contains(expected), qPrintable(errors.join("; ")));
    }

    void saveIsAtomicAndLeavesNoTempFiles() {
        QTemporaryDir dir;
        Config c(dir.filePath("config.json"));
        QString error;
        QVERIFY(c.load(&error));
        ControlRef ref{"key1", "press"};
        QVERIFY2(c.bind("Default", 0, ref, {{"type", "keys"}, {"combo", "ctrl+x"}}, &error), qPrintable(error));
        const auto files = QDir(dir.path()).entryList(QDir::Files);
        QCOMPARE(files, QStringList{"config.json"});
        Config again(dir.filePath("config.json"));
        QVERIFY(again.load(&error));
        QCOMPARE(again.profiles()[0].toObject().value("pages").toArray()[0].toObject().value("bindings").toObject()
                     .value("key1").toObject().value("press").toObject().value("combo").toString(), QString("ctrl+x"));
    }

    void invalidBindIsRejectedAndFileUntouched() {
        QTemporaryDir dir;
        Config c(dir.filePath("config.json"));
        QString error;
        QVERIFY(c.load(&error));
        const QByteArray before = read(dir.filePath("config.json"));
        QVERIFY(!c.bind("Default", 0, {"key1", "press"}, {{"type", "keys"}, {"combo", "ctrl+Nope"}}, &error));
        QVERIFY(error.contains("Nope"));
        QVERIFY(!c.bind("Default", 0, {"key1", "bogus"}, {{"type", "noop"}}, &error));
        QVERIFY(!c.bind("Nobody", 0, {"key1", "press"}, {{"type", "noop"}}, &error));
        QVERIFY(error.contains("no profile"));
        QVERIFY(!c.bind("Default", 5, {"key1", "press"}, {{"type", "noop"}}, &error));
        QCOMPARE(read(dir.filePath("config.json")), before);
    }

    void unbindRemovesSlotOrWholeControl() {
        QTemporaryDir dir;
        Config c(dir.filePath("config.json"));
        QString error;
        QVERIFY(c.load(&error));
        QVERIFY(c.unbind("Default", 0, {"knob", "click"}, false, &error));
        auto knob = [&] { return c.profiles()[0].toObject().value("pages").toArray()[0].toObject().value("bindings").toObject().value("knob").toObject(); };
        QVERIFY(!knob().contains("click"));
        QVERIFY(knob().contains("rotate_cw"));
        QVERIFY(c.unbind("Default", 0, {"knob", ""}, true, &error));
        QVERIFY(knob().isEmpty());
        QVERIFY(!c.unbind("Default", 0, {"knob", "click"}, false, &error));
        QVERIFY(error.contains("not bound"));
    }

    void controlParsing() {
        ControlRef r;
        QString error;
        QVERIFY(Config::parseControl("key4", "", &r, &error));
        QCOMPARE(r.slot, QString("press"));
        QVERIFY(Config::parseControl("key4", "long_press", &r, &error));
        QCOMPARE(r.slot, QString("long_press"));
        QVERIFY(Config::parseControl("knob", "", &r, &error));
        QCOMPARE(r.slot, QString("click"));
        QVERIFY(Config::parseControl("knob.press_rotate_ccw", "", &r, &error));
        QCOMPARE(r.slot, QString("press_rotate_ccw"));
        QVERIFY(Config::parseControl("knob.rotate_cw@key4", "", &r, &error));
        QVERIFY(!Config::parseControl("key8", "", &r, &error));
        QVERIFY(!Config::parseControl("knob.long_press", "", &r, &error));
        QVERIFY(!Config::parseControl("key1.rotate_cw", "", &r, &error));
        QVERIFY(!Config::parseControl("knob.click@key4", "", &r, &error));
        QVERIFY(!Config::parseControl("knob.rotate_cw@knob", "", &r, &error));
    }

    void hotReloadPicksUpExternalEdits() {
        QTemporaryDir dir;
        const QString path = dir.filePath("config.json");
        Config c(path);
        QString error;
        QVERIFY(c.load(&error));
        c.setWatching(true);
        QSignalSpy changed(&c, &Config::changed);
        QJsonObject doc = c.doc();
        QJsonObject device = doc["device"].toObject();
        device["long_press_ms"] = 777;
        doc["device"] = device;
        // An editor-style replace: write beside, then rename over the target.
        write(path + ".tmp", QJsonDocument(doc).toJson());
        QVERIFY(QFile::remove(path));
        QVERIFY(QFile::rename(path + ".tmp", path));
        QTRY_COMPARE_WITH_TIMEOUT(changed.count(), 1, 3000);
        QCOMPARE(c.device().value("long_press_ms").toInt(), 777);
        // and it keeps working for a second edit (the watch must have been re-armed)
        device["long_press_ms"] = 888;
        doc["device"] = device;
        write(path + ".tmp", QJsonDocument(doc).toJson());
        QVERIFY(QFile::remove(path));
        QVERIFY(QFile::rename(path + ".tmp", path));
        QTRY_COMPARE_WITH_TIMEOUT(changed.count(), 2, 3000);
        QCOMPARE(c.device().value("long_press_ms").toInt(), 888);
    }

    void ownSavesDoNotEchoAsExternalChanges() {
        QTemporaryDir dir;
        Config c(dir.filePath("config.json"));
        QString error;
        QVERIFY(c.load(&error));
        c.setWatching(true);
        QSignalSpy changed(&c, &Config::changed);
        QVERIFY(c.bind("Default", 0, {"key1", "press"}, {{"type", "noop"}}, &error));
        QCOMPARE(changed.count(), 1);                 // the explicit emit from bind()
        QTest::qWait(500);
        QCOMPARE(changed.count(), 1);                 // and nothing from the file watcher
    }

    void brokenEditKeepsPreviousConfigAndReports() {
        QTemporaryDir dir;
        const QString path = dir.filePath("config.json");
        Config c(path);
        QString error;
        QVERIFY(c.load(&error));
        c.setWatching(true);
        QSignalSpy failed(&c, &Config::loadFailed);
        QSignalSpy changed(&c, &Config::changed);
        write(path + ".tmp", "{ nope");
        QVERIFY(QFile::remove(path));
        QVERIFY(QFile::rename(path + ".tmp", path));
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 3000);
        QCOMPARE(changed.count(), 0);
        QCOMPARE(c.profiles().size(), 1);
    }
};

class ShortcutTests : public QObject {
    Q_OBJECT
private slots:
    void parsesAndCanonicalises_data() {
        QTest::addColumn<QString>("input");
        QTest::addColumn<QString>("canonical");
        QTest::newRow("basic") << "ctrl+shift+z" << "ctrl+shift+z";
        QTest::newRow("case and order") << "Shift+CTRL+Z" << "ctrl+shift+z";
        QTest::newRow("super") << "super+1" << "super+1";
        QTest::newRow("win alias") << "win+Tab" << "super+Tab";
        QTest::newRow("plus key") << "ctrl++" << "ctrl+plus";
        QTest::newRow("named plus") << "ctrl+plus" << "ctrl+plus";
        QTest::newRow("media") << "XF86AudioMute" << "XF86AudioMute";
        QTest::newRow("arrow") << "super+shift+left" << "shift+super+Left";
        QTest::newRow("esc") << "alt+esc" << "alt+Escape";
        QTest::newRow("bare key") << "F5" << "F5";
    }
    void parsesAndCanonicalises() {
        QFETCH(QString, input);
        QFETCH(QString, canonical);
        Combo c;
        QString error;
        QVERIFY2(Shortcut::parse(input, &c, &error), qPrintable(error));
        QCOMPARE(c.toString(), canonical);
    }
    void rejectsTypos() {
        Combo c;
        QString error;
        QVERIFY(!Shortcut::parse("ctrl+Retrun", &c, &error));
        QVERIFY(error.contains("Retrun"));
        QVERIFY(!Shortcut::parse("ctlr+c", &c, &error));
        QVERIFY(error.contains("ctlr"));
        QVERIFY(!Shortcut::parse("", &c, &error));
        QVERIFY(!Shortcut::parse("ctrl+", &c, &error));
    }
    void wtypeAndHyprSpelling() {
        Combo c;
        QString error;
        QVERIFY(Shortcut::parse("ctrl+shift+z", &c, &error));
        QCOMPARE(c.wtypeArgs(), (QStringList{"-M", "ctrl", "-M", "shift", "-k", "z", "-m", "shift", "-m", "ctrl"}));
        QCOMPARE(c.hyprMods(), QString("CTRL SHIFT"));
        QVERIFY(Shortcut::parse("super+1", &c, &error));
        QCOMPARE(c.wtypeArgs(), (QStringList{"-M", "logo", "-k", "1", "-m", "logo"}));
        QCOMPARE(c.hyprMods(), QString("SUPER"));
    }
    void modmaskDecoding() {
        QCOMPARE(Shortcut::modsFromMask(0), QStringList());
        QCOMPARE(Shortcut::modsFromMask(64), (QStringList{"super"}));
        QCOMPARE(Shortcut::modsFromMask(1 | 64), (QStringList{"shift", "super"}));
        QCOMPARE(Shortcut::modsFromMask(4 | 8), (QStringList{"ctrl", "alt"}));
        QCOMPARE(Shortcut::modsFromMask(77), (QStringList{"ctrl", "shift", "alt", "super"}));
    }
    void hyprBindsParsing() {
        const QByteArray json = R"([
          {"mouse":false,"modmask":64,"key":"SPACE","description":"Launch apps","dispatcher":"__lua","arg":"3"},
          {"mouse":false,"modmask":0,"key":"XF86AudioRaiseVolume","description":"Volume up","dispatcher":"__lua","arg":"12"},
          {"mouse":true,"modmask":64,"key":"mouse:272","description":"Move window","dispatcher":"__lua","arg":"1"},
          {"mouse":false,"modmask":65,"key":"code:10","description":"Keycode only","dispatcher":"__lua","arg":"2"},
          {"mouse":false,"modmask":5,"key":"Z","description":"","dispatcher":"__lua","arg":"4"}
        ])";
        const auto binds = Shortcut::parseHyprBinds(json);
        QCOMPARE(binds.size(), 5);
        QCOMPARE(binds[0].combo, QString("super+space"));
        QVERIFY(binds[0].usable);
        QCOMPARE(binds[1].combo, QString("XF86AudioRaiseVolume"));
        QVERIFY(!binds[2].usable);
        QVERIFY(!binds[3].usable);
        QCOMPARE(binds[4].combo, QString("ctrl+shift+z"));
    }
    void terminalKeyDecoding() {
        Combo c;
        QString error;
        QVERIFY(Shortcut::fromTerminalInput("\x1b[122;6u", &c, &error));     // kitty: ctrl+shift+z
        QCOMPARE(c.toString(), QString("ctrl+shift+z"));
        QVERIFY(Shortcut::fromTerminalInput("\x1b[1;5C", &c, &error));       // ctrl+right
        QCOMPARE(c.toString(), QString("ctrl+Right"));
        QVERIFY(Shortcut::fromTerminalInput("\x03", &c, &error));            // legacy ctrl+c
        QCOMPARE(c.toString(), QString("ctrl+c"));
        QVERIFY(Shortcut::fromTerminalInput("\x1b" "x", &c, &error));        // alt+x
        QCOMPARE(c.toString(), QString("alt+x"));
        QVERIFY(Shortcut::fromTerminalInput("\x1b[3;3~", &c, &error));       // alt+Delete
        QCOMPARE(c.toString(), QString("alt+Delete"));
        QVERIFY(!Shortcut::fromTerminalInput("", &c, &error));
    }
};

REGISTER_TEST(ConfigTests)
REGISTER_TEST(ShortcutTests)
#include "tests_config.moc"
