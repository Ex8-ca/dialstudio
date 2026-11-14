#include "testmain.h"

#include <QDir>
#include <QLocalServer>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "config.h"
#include "profiles.h"

class ProfileTests : public QObject {
    Q_OBJECT

    static QJsonArray sample() {
        auto page = [](const char *name, const char *combo) {
            return QJsonObject{{"name", name},
                               {"bindings", QJsonObject{{"key1", QJsonObject{{"press", QJsonObject{{"type", "keys"}, {"combo", combo}}}}}}}};
        };
        return QJsonArray{
            QJsonObject{{"name", "Default"}, {"match", QJsonValue::Null}, {"pages", QJsonArray{page("Main", "ctrl+c"), page("Two", "ctrl+v")}}},
            QJsonObject{{"name", "Kdenlive"}, {"match", QJsonObject{{"class", "^org\\.kde\\.kdenlive$"}}}, {"pages", QJsonArray{page("Edit", "k")}}},
            QJsonObject{{"name", "Browser"}, {"match", QJsonObject{{"class", "firefox|chrome"}, {"title", "GitHub"}}}, {"pages", QJsonArray{page("Web", "ctrl+t")}}},
        };
    }
    static void seed(const QString &path) {
        QJsonObject doc = Config::defaults();
        doc["profiles"] = sample();
        QFile f(path);
        f.open(QIODevice::WriteOnly);
        f.write(QJsonDocument(doc).toJson());
    }

private slots:
    void resolveByClassAndTitleWithFallback() {
        QCOMPARE(Profiles::resolve(sample(), "org.kde.kdenlive", ""), QString("Kdenlive"));
        QCOMPARE(Profiles::resolve(sample(), "org.kde.kdenlive2", ""), QString("Default"));    // anchored regex
        QCOMPARE(Profiles::resolve(sample(), "google-chrome", "GitHub - x"), QString("Browser"));
        QCOMPARE(Profiles::resolve(sample(), "google-chrome", "YouTube"), QString("Default")); // title must match too
        QCOMPARE(Profiles::resolve(sample(), "", ""), QString("Default"));
    }

    void activeWindowSwitchesBindingsAndPinOverrides() {
        QTemporaryDir dir;
        seed(dir.filePath("c.json"));
        Config config(dir.filePath("c.json"));
        QString e;
        QVERIFY2(config.load(&e), qPrintable(e));
        Profiles p(&config);
        QSignalSpy changed(&p, &Profiles::changed);
        QCOMPARE(p.activeName(), QString("Default"));
        QCOMPARE(p.lookup("key1", "press").value("combo").toString(), QString("ctrl+c"));

        p.setActiveWindow("org.kde.kdenlive", "x");
        QCOMPARE(p.activeName(), QString("Kdenlive"));
        QCOMPARE(p.lookup("key1", "press").value("combo").toString(), QString("k"));
        QCOMPARE(changed.count(), 1);

        p.setActiveWindow("org.kde.kdenlive", "other title");     // same profile: no spurious change
        QCOMPARE(changed.count(), 1);

        QVERIFY(p.pin("Default"));
        QVERIFY(p.pinned());
        p.setActiveWindow("org.kde.kdenlive", "again");
        QCOMPARE(p.activeName(), QString("Default"));             // pinned wins over the window
        QVERIFY(!p.pin("Nope"));
        p.unpin();
        QCOMPARE(p.activeName(), QString("Kdenlive"));
    }

    void pagesWrapAndAreRememberedPerProfile() {
        QTemporaryDir dir;
        seed(dir.filePath("c.json"));
        Config config(dir.filePath("c.json"));
        QString e;
        QVERIFY(config.load(&e));
        Profiles p(&config);
        QCOMPARE(p.pageCount(), 2);
        QVERIFY(p.nextPage(1));
        QCOMPARE(p.pageName(), QString("Two"));
        QCOMPARE(p.lookup("key1", "press").value("combo").toString(), QString("ctrl+v"));
        QVERIFY(p.nextPage(1));
        QCOMPARE(p.page(), 0);                                    // wraps
        QVERIFY(p.nextPage(-1));
        QCOMPARE(p.page(), 1);
        QVERIFY(!p.setPage(9));
        p.setActiveWindow("org.kde.kdenlive", "");
        QCOMPARE(p.page(), 0);
        p.setActiveWindow("", "");
        QCOMPARE(p.page(), 1);                                    // Default remembered page 1
    }

    void configChangeRefreshesActiveProfile() {
        QTemporaryDir dir;
        seed(dir.filePath("c.json"));
        Config config(dir.filePath("c.json"));
        QString e;
        QVERIFY(config.load(&e));
        Profiles p(&config);
        p.setActiveWindow("org.kde.kdenlive", "");
        QVERIFY(config.bind("Kdenlive", 0, {"key1", "press"}, {{"type", "keys"}, {"combo", "j"}}, &e));
        QCOMPARE(p.lookup("key1", "press").value("combo").toString(), QString("j"));
    }

    void hyprlandEventLineParsing() {
        QString c, t;
        QVERIFY(HyprlandWatcher::parseLine("activewindow>>kitty,~/Work - nvim, a,b", &c, &t));
        QCOMPARE(c, QString("kitty"));
        QCOMPARE(t, QString("~/Work - nvim, a,b"));
        QVERIFY(HyprlandWatcher::parseLine("activewindow>>,", &c, &t));
        QCOMPARE(c, QString());
        QVERIFY(!HyprlandWatcher::parseLine("workspace>>2", &c, &t));
        QVERIFY(!HyprlandWatcher::parseLine("activewindowv2>>0x1234", &c, &t));
    }

    void socketLookupFallsBackToNewestInstance() {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("hypr/older");
        QDir(dir.path()).mkpath("hypr/newer");
        for (const char *name : {"older", "newer"}) {
            QFile f(dir.filePath(QString("hypr/%1/.socket2.sock").arg(name)));
            f.open(QIODevice::WriteOnly);
            f.close();
            QTest::qWait(1100);                                   // distinct mtimes
        }
        QString sig;
        QCOMPARE(QFileInfo(HyprlandWatcher::findSocket(dir.path(), "", &sig)).dir().dirName(), QString("newer"));
        QCOMPARE(sig, QString("newer"));
        HyprlandWatcher::findSocket(dir.path(), "older", &sig);
        QCOMPARE(sig, QString("older"));                          // the env signature wins when its socket exists
        QVERIFY(HyprlandWatcher::findSocket(dir.path() + "/none", "", &sig).isEmpty());
    }

    void watcherReceivesEventsFromARealSocket() {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("hypr/sig1");
        const QString path = dir.filePath("hypr/sig1/.socket2.sock");
        QLocalServer server;
        QVERIFY(server.listen(path));
        qputenv("XDG_RUNTIME_DIR", dir.path().toUtf8());
        qputenv("HYPRLAND_INSTANCE_SIGNATURE", "sig1");
        HyprlandWatcher watcher;
        QSignalSpy spy(&watcher, &HyprlandWatcher::windowChanged);
        watcher.start();
        QVERIFY(server.waitForNewConnection(2000));
        QLocalSocket *client = server.nextPendingConnection();
        client->write("workspace>>1\nactivewindow>>foot,hello\nactivewindow>>org.kde.kdenlive,Project");
        client->flush();
        QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 2000);          // partial last line is held back
        client->write("\n");
        client->flush();
        QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 2, 2000);
        QCOMPARE(spy[1][0].toString(), QString("org.kde.kdenlive"));
        QCOMPARE(spy[1][1].toString(), QString("Project"));
    }
};

REGISTER_TEST(ProfileTests)
#include "tests_profiles.moc"
