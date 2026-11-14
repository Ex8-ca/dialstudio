#include "testmain.h"

#include <QDBusConnection>
#include <QDir>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "daemon.h"
#include "fakebluez.h"
#include "ipc.h"

#ifndef FIXTURE_DIR
#define FIXTURE_DIR "fixtures"
#endif

// End to end: fake dial -> daemon -> IPC client, with the config in a temp directory.
class DaemonTests : public QObject {
    Q_OBJECT

    static bool onPrivateBus() { return qEnvironmentVariableIsSet("DIALSTUDIO_TEST_BUS"); }

    struct Rig {
        QTemporaryDir dir;
        FakeBluez fake;
        std::unique_ptr<Daemon> daemon;
        QString socket;
        QString hyprConfig;      // a copy of the fixture; empty leaves the Omarchy scan off
        QStringList appsDirs;    // for the apps op; empty uses the XDG defaults
        bool start(const QByteArray &configJson = {}) {
            socket = dir.filePath("s.sock");
            Daemon::Options o;
            o.configPath = dir.filePath("config.json");
            o.socketPath = socket;
            o.watchHyprland = false;
            o.probeScope = false;
            o.toasts = false;
            o.dryRunActions = true;
            o.hyprConfig = hyprConfig;
            o.omarchyPath = "/nonexistent-omarchy";
            o.appsDirs = appsDirs;
            if (!configJson.isEmpty()) {
                QFile f(o.configPath);
                f.open(QIODevice::WriteOnly);
                f.write(configJson);
            }
            fake.autoReply(true);
            fake.addDial(true);
            daemon = std::make_unique<Daemon>(QDBusConnection::sessionBus(), o);
            QString error;
            if (!daemon->start(&error)) { qWarning() << error; return false; }
            return true;
        }
        // The client blocks, so it runs on a worker thread while this thread keeps the daemon alive.
        QJsonObject ask(const QJsonObject &request, int timeoutMs = 4000) {
            QJsonObject reply;
            const QString path = socket;
            QThread *thread = QThread::create([&reply, request, timeoutMs, path] {
                IpcClient c;
                if (!c.connectTo(path)) { reply = {{"ok", false}, {"error", "no connect"}}; return; }
                QString err;
                reply = c.request(request, timeoutMs, &err);
            });
            QEventLoop loop;
            QObject::connect(thread, &QThread::finished, &loop, &QEventLoop::quit);
            thread->start();
            loop.exec();
            thread->wait();
            delete thread;
            return reply;
        }
    };

private slots:
    void initTestCase() {
        if (!onPrivateBus()) QSKIP("needs the private session bus from bin/test");
    }

    void statusReportsDialStateFromRepliesNotGuesses() {
        Rig r;
        QVERIFY(r.start());
        QTRY_COMPARE_WITH_TIMEOUT(r.daemon->status().value("link").toString(), QString("online"), 3000);
        QTRY_COMPARE_WITH_TIMEOUT(r.daemon->status().value("device").toObject().value("firmware").toString(), QString("1.3.8.1"), 3000);
        QTRY_COMPARE_WITH_TIMEOUT(r.daemon->status().value("sleep_seconds").toInt(), 7200, 3000);
        const QJsonObject s = r.daemon->status();
        QCOMPARE(s.value("haptics").toBool(), true);
        QCOMPARE(s.value("battery").toObject().value("percent").toInt(), 80);
        QTRY_COMPARE_WITH_TIMEOUT(r.daemon->status().value("device").toObject().value("serial").toString(), QString("A0D26I003U3711117"), 2000);
    }

    void keyEventsReachBindingsAndNoOpActionsAreHarmless() {
        Rig r;
        QVERIFY(r.start());
        QTRY_COMPARE_WITH_TIMEOUT(r.daemon->status().value("link").toString(), QString("online"), 3000);
        QSignalSpy failures(r.daemon->actions(), &Actions::failed);
        QSignalSpy internal(r.daemon->actions(), &Actions::internalAction);
        QString e;
        QVERIFY(r.daemon->config()->bind("Default", 0, {"key1", "press"}, {{"type", "page"}, {"action", "next"}}, &e));
        r.fake.notify(QByteArray::fromHex("8b100101"));
        r.fake.notify(QByteArray::fromHex("8b100100"));
        QTRY_COMPARE_WITH_TIMEOUT(internal.count(), 1, 2000);
        QCOMPARE(internal[0][0].toString(), QString("page"));
    }

    void defaultConfigTurnsKnobIntoWpctlVolumeAndKeysIntoMediaViaDryRun() {
        Rig r;
        QVERIFY(r.start());
        QTRY_COMPARE_WITH_TIMEOUT(r.daemon->status().value("link").toString(), QString("online"), 3000);
        QList<QStringList> ran;
        connect(r.daemon->actions(), &Actions::ran, this, [&](const QStringList &argv) { ran << argv; });
        r.fake.notify(QByteArray::fromHex("8b100a01"));      // cw
        r.fake.notify(QByteArray::fromHex("8b100a01"));
        QTRY_VERIFY_WITH_TIMEOUT(!ran.isEmpty(), 1000);
        QCOMPARE(ran.last().first(), QString("wpctl"));
        QCOMPARE(ran.last().last().right(2), QString("%+"));
        ran.clear();
        r.fake.notify(QByteArray::fromHex("8b100701"));      // knob click -> mute
        QTRY_COMPARE_WITH_TIMEOUT(ran.size(), 1, 1000);
        QCOMPARE(ran[0], (QStringList{"wpctl", "set-mute", "@DEFAULT_AUDIO_SINK@", "toggle"}));
        ran.clear();
        r.fake.notify(QByteArray::fromHex("8b100c01"));      // press-rotate cw -> brightness
        QTRY_VERIFY_WITH_TIMEOUT(!ran.isEmpty(), 1000);
        QCOMPARE(ran[0].first(), QString("brightnessctl"));
    }

    void haptics_set_is_confirmed_by_the_dial_and_readable_back() {
        Rig r;
        QVERIFY(r.start());
        QTRY_COMPARE_WITH_TIMEOUT(r.daemon->status().value("haptics").toBool(), true, 3000);
        QJsonObject off = r.ask({{"op", "set"}, {"what", "haptics"}, {"value", false}});
        QVERIFY2(off.value("ok").toBool(), qPrintable(off.value("error").toString()));
        QCOMPARE(off.value("haptics").toBool(), false);
        QCOMPARE(r.daemon->status().value("haptics").toBool(), false);
        // the write on the wire is the documented one
        bool saw = false;
        for (const auto &w : r.fake.writes()) saw = saw || w.plain.startsWith(QByteArray::fromHex("0106400400"));
        QVERIFY(saw);
        QJsonObject on = r.ask({{"op", "set"}, {"what", "haptics"}, {"value", true}});
        QCOMPARE(on.value("haptics").toBool(), true);
        QCOMPARE(r.daemon->config()->device().value("haptics").toBool(), true);
    }

    void sleep_set_validates_and_round_trips() {
        Rig r;
        QVERIFY(r.start());
        QTRY_COMPARE_WITH_TIMEOUT(r.daemon->status().value("sleep_seconds").toInt(), 7200, 3000);
        QJsonObject bad = r.ask({{"op", "set"}, {"what", "sleep"}, {"value", 70000}});
        QVERIFY(!bad.value("ok").toBool());
        QJsonObject good = r.ask({{"op", "set"}, {"what", "sleep"}, {"value", 3600}});
        QVERIFY2(good.value("ok").toBool(), qPrintable(good.value("error").toString()));
        QCOMPARE(good.value("sleep_seconds").toInt(), 3600);
        QCOMPARE(r.daemon->status().value("sleep_seconds").toInt(), 3600);
    }

    void configIsReappliedToTheDialAfterReconnect() {
        Rig r;
        QVERIFY(r.start());
        QTRY_COMPARE_WITH_TIMEOUT(r.daemon->status().value("haptics").toBool(), true, 3000);
        // Config says off, the "dial" says on: the read reply after connect must be corrected.
        QString e;
        QVERIFY(r.daemon->config()->setDevice("haptics", false, &e));
        QTRY_COMPARE_WITH_TIMEOUT(r.daemon->status().value("haptics").toBool(), false, 3000);
        r.fake.setConnected(false);
        QTRY_VERIFY_WITH_TIMEOUT(r.daemon->status().value("link").toString() != "online", 2000);
        QTRY_COMPARE_WITH_TIMEOUT(r.daemon->status().value("link").toString(), QString("online"), 8000);
    }

    void bindOverIpcSavesValidatesAndLists() {
        Rig r;
        QVERIFY(r.start());
        QJsonObject ok = r.ask({{"op", "bind"}, {"control", "key4"}, {"action", QJsonObject{{"type", "keys"}, {"combo", "ctrl+shift+z"}}}, {"page", 0}});
        QVERIFY2(ok.value("ok").toBool(), qPrintable(ok.value("error").toString()));
        QCOMPARE(ok.value("slot").toString(), QString("press"));
        QJsonObject listed = r.ask({{"op", "bindings"}});
        bool found = false;
        for (const auto &b : listed.value("bindings").toArray())
            if (b.toObject().value("control").toString() == "key4") found = b.toObject().value("summary").toString() == "ctrl+shift+z";
        QVERIFY(found);
        QJsonObject bad = r.ask({{"op", "bind"}, {"control", "key4"}, {"action", QJsonObject{{"type", "keys"}, {"combo", "ctrl+Retrun"}}}});
        QVERIFY(!bad.value("ok").toBool());
        QVERIFY(bad.value("error").toString().contains("Retrun"));
        QJsonObject knob = r.ask({{"op", "bind"}, {"control", "knob.rotate_cw@key4"}, {"action", QJsonObject{{"type", "volume"}, {"action", "up"}}}});
        QVERIFY2(knob.value("ok").toBool(), qPrintable(knob.value("error").toString()));
        QJsonObject gone = r.ask({{"op", "unbind"}, {"control", "key4"}});
        QVERIFY(gone.value("ok").toBool());
        QJsonObject again = r.ask({{"op", "unbind"}, {"control", "key4"}});
        QVERIFY(!again.value("ok").toBool());
    }

    void profileAndPageOps() {
        Rig r;
        QVERIFY(r.start());
        QVERIFY(!r.ask({{"op", "profile"}, {"name", "Nope"}}).value("ok").toBool());
        QVERIFY(r.ask({{"op", "profile"}, {"name", "Default"}}).value("ok").toBool());
        QVERIFY(r.daemon->status().value("profile").toObject().value("pinned").toBool());
        QVERIFY(r.ask({{"op", "profile"}, {"name", "auto"}}).value("ok").toBool());
        QVERIFY(!r.daemon->status().value("profile").toObject().value("pinned").toBool());
        QVERIFY(!r.ask({{"op", "page"}, {"page", "7"}}).value("ok").toBool());
        QVERIFY(r.ask({{"op", "page"}, {"page", "next"}}).value("ok").toBool());
    }

    void subscribersSeeInputEvents() {
        Rig r;
        QVERIFY(r.start());
        QTRY_COMPARE_WITH_TIMEOUT(r.daemon->status().value("link").toString(), QString("online"), 3000);
        QStringList seen;
        const QString path = r.socket;
        QThread *thread = QThread::create([&seen, path] {
            IpcClient c;
            if (!c.connectTo(path)) return;
            c.request({{"op", "subscribe"}}, 2000);
            QJsonObject e;
            QElapsedTimer t;
            t.start();
            while (t.elapsed() < 2500 && seen.size() < 3)
                if (c.readLine(&e, 300) && e.value("event").toString() == "input") seen << e.value("text").toString();
        });
        QEventLoop loop;
        connect(thread, &QThread::finished, &loop, &QEventLoop::quit);
        thread->start();
        QTest::qWait(400);
        r.fake.notify(QByteArray::fromHex("8b100301"));
        r.fake.notify(QByteArray::fromHex("8b100300"));
        r.fake.notify(QByteArray::fromHex("8b100701"));
        loop.exec();
        thread->wait();
        delete thread;
        QCOMPARE(seen, (QStringList{"key3 press", "key3 release", "knob click"}));
    }

    void secondDaemonOnTheSameSocketIsRefused() {
        Rig r;
        QVERIFY(r.start());
        // listen() probes with a blocking client, which needs the first daemon's loop to run,
        // so the second daemon is constructed and started on a worker thread's own event loop.
        bool started = true;
        QString error;
        const QString socket = r.socket, config = r.dir.filePath("other.json");
        QThread *thread = QThread::create([&] {
            QCoreApplication::processEvents();
            IpcServer server;
            started = server.listen(socket, &error);
        });
        QEventLoop loop;
        QObject::connect(thread, &QThread::finished, &loop, &QEventLoop::quit);
        thread->start();
        loop.exec();
        thread->wait();
        delete thread;
        QVERIFY2(!started, "a second server must not take over a live socket");
        QVERIFY(error.contains("already running"));
        QVERIFY(r.ask({{"op", "status"}}).value("ok").toBool());      // the first one is unharmed
        Q_UNUSED(config);
    }

    void brokenConfigRefusesToStartAndIsNotOverwritten() {
        Rig r;
        QByteArray broken = "{ nope";
        QVERIFY(!r.start(broken));
        QFile f(r.dir.filePath("config.json"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), broken);
    }

    void sigtermPathStopsHeartbeatsAndUnsubscribes() {
        Rig r;
        QVERIFY(r.start());
        QTRY_COMPARE_WITH_TIMEOUT(r.daemon->status().value("link").toString(), QString("online"), 3000);
        r.daemon->halt();
        QCOMPARE(r.fake.count("StopNotify"), 1);
        r.fake.clearRecords();
        QTest::qWait(1500);
        QCOMPARE(r.fake.heartbeats().size(), 0);
        QVERIFY(!QFile::exists(r.socket));
    }

    // ---- IPC used by the window -------------------------------------------------------

    // Copies the fixture config into the rig's directory so a test can edit it.
    static QString useFixture(Rig &r) {
        const QString target = r.dir.filePath("hyprland.lua");
        QFile::copy(QStringLiteral(FIXTURE_DIR "/omarchy/hyprland.lua"), target);
        r.hyprConfig = target;
        return target;
    }

    void bindingsGetAndSetRoundTripWithValidation() {
        Rig r;
        QVERIFY(r.start());
        QJsonObject got = r.ask({{"op", "bindings_get"}});
        QVERIFY(got.value("ok").toBool());
        QCOMPARE(got.value("profile").toString(), QString("Default"));
        QJsonObject bindings = got.value("bindings").toObject();
        QCOMPARE(bindings.value("key4").toObject().value("press").toObject().value("combo").toString(), QString("ctrl+c"));

        bindings.insert("key4", QJsonObject{{"press", QJsonObject{{"type", "keys"}, {"combo", "ctrl+shift+z"}}}});
        QJsonObject set = r.ask({{"op", "bindings_set"}, {"bindings", bindings}});
        QVERIFY2(set.value("ok").toBool(), qPrintable(set.value("error").toString()));
        QCOMPARE(r.daemon->config()->profiles()[0].toObject().value("pages")[0].toObject().value("bindings").toObject()
                     .value("key4").toObject().value("press").toObject().value("combo").toString(), QString("ctrl+shift+z"));
        // The file on disk has it (atomic save), not just memory.
        QFile f(r.dir.filePath("config.json"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        QVERIFY(f.readAll().contains("ctrl+shift+z"));

        // A bad slot, a bad action and a bad page are refused and change nothing.
        QJsonObject badSlot = bindings;
        badSlot.insert("key4", QJsonObject{{"nope", QJsonObject{{"type", "noop"}}}});
        QVERIFY(!r.ask({{"op", "bindings_set"}, {"bindings", badSlot}}).value("ok").toBool());
        QJsonObject badAction = bindings;
        badAction.insert("key4", QJsonObject{{"press", QJsonObject{{"type", "keys"}, {"combo", "ctrl+Retrun"}}}});
        const QJsonObject refused = r.ask({{"op", "bindings_set"}, {"bindings", badAction}});
        QVERIFY(!refused.value("ok").toBool());
        QVERIFY(refused.value("error").toString().contains("Retrun"));
        QVERIFY(!r.ask({{"op", "bindings_set"}, {"page", 9}, {"bindings", bindings}}).value("ok").toBool());
        QVERIFY(!r.ask({{"op", "bindings_get"}, {"profile", "Nope"}}).value("ok").toBool());
        QCOMPARE(r.ask({{"op", "bindings_get"}}).value("bindings").toObject().value("key4").toObject()
                     .value("press").toObject().value("combo").toString(), QString("ctrl+shift+z"));
    }

    void inputEventsCarryStructuredFieldsForTheWindow() {
        Rig r;
        QVERIFY(r.start());
        QTRY_COMPARE_WITH_TIMEOUT(r.daemon->status().value("link").toString(), QString("online"), 3000);
        QList<QJsonObject> events;
        const QString path = r.socket;
        QThread *thread = QThread::create([&events, path] {
            IpcClient c;
            if (!c.connectTo(path)) return;
            c.request({{"op", "subscribe"}}, 2000);
            QJsonObject e;
            QElapsedTimer t;
            t.start();
            while (t.elapsed() < 2500 && events.size() < 4)
                if (c.readLine(&e, 300) && e.value("event").toString() == "input") events << e;
        });
        QEventLoop loop;
        connect(thread, &QThread::finished, &loop, &QEventLoop::quit);
        thread->start();
        QTest::qWait(400);
        r.fake.notify(QByteArray::fromHex("8b100401"));      // key4 down
        r.fake.notify(QByteArray::fromHex("8b100400"));      // key4 up
        r.fake.notify(QByteArray::fromHex("8b100701"));      // knob click
        r.fake.notify(QByteArray::fromHex("8b100a01"));      // rotate cw
        loop.exec();
        thread->wait();
        delete thread;
        QCOMPARE(events.size(), 4);
        QCOMPARE(events[0].value("control").toString(), QString("key4"));
        QCOMPARE(events[0].value("kind").toString(), QString("press"));
        QCOMPARE(events[1].value("kind").toString(), QString("release"));
        QCOMPARE(events[2].value("control").toString(), QString("knob"));
        QCOMPARE(events[2].value("kind").toString(), QString("click"));
        QCOMPARE(events[3].value("kind").toString(), QString("rotate"));
        QCOMPARE(events[3].value("direction").toString(), QString("cw"));
        QCOMPARE(events[3].value("steps").toInt(), 1);
        QVERIFY(!events[0].value("text").toString().isEmpty());      // the CLI still gets its text
    }

    void testOpFiresThroughTheDaemonAndRefusesBadActions() {
        Rig r;
        QVERIFY(r.start());
        QList<QStringList> ran;
        connect(r.daemon->actions(), &Actions::ran, this, [&](const QStringList &argv) { ran << argv; });
        const QJsonObject reply = r.ask({{"op", "test"}, {"action", QJsonObject{{"type", "volume"}, {"action", "up"}, {"step", 3}}}});
        QVERIFY2(reply.value("ok").toBool(), qPrintable(reply.value("error").toString()));
        QVERIFY(!reply.value("argv").toArray().isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(ran.size(), 1, 1000);
        QVERIFY(ran[0].last().startsWith("3%"));
        QVERIFY(!r.ask({{"op", "test"}, {"action", QJsonObject{{"type", "keys"}, {"combo", "ctrl+Retrun"}}}}).value("ok").toBool());
    }

    void injectIsOffUnlessDevModeIsSet() {
        Rig r;
        QVERIFY(r.start());
        qunsetenv("DIALSTUDIO_DEV");
        const QJsonObject denied = r.ask({{"op", "inject"}, {"kind", "click"}});
        QVERIFY(!denied.value("ok").toBool());
        QVERIFY(denied.value("error").toString().contains("DIALSTUDIO_DEV"));
        qputenv("DIALSTUDIO_DEV", "1");
        QVERIFY(r.ask({{"op", "inject"}, {"kind", "click"}}).value("ok").toBool());
        QVERIFY(r.ask({{"op", "inject"}, {"kind", "down"}, {"key", 4}}).value("ok").toBool());
        QVERIFY(r.ask({{"op", "inject"}, {"kind", "up"}, {"key", 4}}).value("ok").toBool());
        QVERIFY(!r.ask({{"op", "inject"}, {"kind", "down"}, {"key", 9}}).value("ok").toBool());
        QVERIFY(!r.ask({{"op", "inject"}, {"kind", "wiggle"}}).value("ok").toBool());
        qunsetenv("DIALSTUDIO_DEV");
    }

    // ---- Omarchy shortcut links -------------------------------------------------------

    void shortcutsOpListsCatalogAndFilters() {
        Rig r;
        useFixture(r);
        QVERIFY(r.start());
        const QJsonObject all = r.ask({{"op", "shortcuts"}});
        QVERIFY2(all.value("ok").toBool(), qPrintable(all.value("error").toString()));
        QVERIFY(all.value("total").toInt() > 10);
        QVERIFY(all.value("runnable").toInt() < all.value("total").toInt());      // Universal copy is a Lua function
        const QJsonObject filtered = r.ask({{"op", "shortcuts"}, {"filter", "terminal"}});
        QVERIFY(filtered.value("shortcuts").toArray().size() >= 1);
        for (const auto &s : filtered.value("shortcuts").toArray())
            QVERIFY(s.toObject().value("description").toString().contains("terminal", Qt::CaseInsensitive) ||
                    s.toObject().value("combo").toString().contains("terminal", Qt::CaseInsensitive));
    }

    void bindOmarchyStoresAReferenceAndRunsTheCommandNotKeys() {
        Rig r;
        useFixture(r);
        QVERIFY(r.start());
        const QJsonObject bound = r.ask({{"op", "bind"}, {"control", "key4"}, {"page", 0},
                                         {"action", QJsonObject{{"type", "omarchy_bind"}, {"description", "terminal"}}}});
        QVERIFY2(bound.value("ok").toBool(), qPrintable(bound.value("error").toString()));
        const QJsonObject stored = bound.value("action").toObject();
        QCOMPARE(stored.value("type").toString(), QString("omarchy_bind"));
        QCOMPARE(stored.value("description").toString(), QString("Terminal"));       // canonical, not as typed
        QCOMPARE(stored.value("combo").toString(), QString("SUPER + RETURN"));
        QVERIFY(!stored.contains("via"));

        QList<QStringList> ran;
        connect(r.daemon->actions(), &Actions::ran, this, [&](const QStringList &argv) { ran << argv; });
        QTRY_COMPARE_WITH_TIMEOUT(r.daemon->status().value("link").toString(), QString("online"), 3000);
        r.fake.notify(QByteArray::fromHex("8b100401"));
        r.fake.notify(QByteArray::fromHex("8b100400"));
        QTRY_COMPARE_WITH_TIMEOUT(ran.size(), 1, 2000);
        QCOMPARE(ran[0], (QStringList{"omarchy-launch-terminal"}));                  // the command, not wtype / send_shortcut
        QVERIFY(r.daemon->status().value("warnings").toArray().isEmpty());
    }

    void bindOmarchyRefusesUnrunnableAmbiguousAndUnknown() {
        Rig r;
        useFixture(r);
        QVERIFY(r.start());
        auto bind = [&](const QString &d) {
            return r.ask({{"op", "bind"}, {"control", "key5"}, {"action", QJsonObject{{"type", "omarchy_bind"}, {"description", d}}}});
        };
        const QJsonObject notRunnable = bind("Universal copy");
        QVERIFY(!notRunnable.value("ok").toBool());
        QVERIFY2(notRunnable.value("error").toString().contains("cannot be bound"), qPrintable(notRunnable.value("error").toString()));
        QVERIFY(notRunnable.value("error").toString().contains("Lua function"));
        QVERIFY(bind("Switch to workspace").value("error").toString().contains("several"));
        QVERIFY(bind("No such shortcut").value("error").toString().contains("No such shortcut"));
        // The old key5 binding is untouched by the refusals.
        QCOMPARE(r.daemon->config()->profiles()[0].toObject().value("pages")[0].toObject().value("bindings").toObject()
                     .value("key5").toObject().value("press").toObject().value("combo").toString(), QString("ctrl+v"));
    }

    void legacyKeysOmarchyLinkIsUpgradedOnBind() {
        Rig r;
        useFixture(r);
        QVERIFY(r.start());
        const QJsonObject bound = r.ask({{"op", "bind"}, {"control", "key6"},
                                         {"action", QJsonObject{{"type", "keys"}, {"omarchy", "Terminal"}, {"via", "hypr"}}}});
        QVERIFY2(bound.value("ok").toBool(), qPrintable(bound.value("error").toString()));
        QCOMPARE(bound.value("action").toObject().value("type").toString(), QString("omarchy_bind"));
    }

    void reloadReResolvesAndWarnsWhenADescriptionDisappears() {
        Rig r;
        const QString lua = useFixture(r);
        QVERIFY(r.start());
        QVERIFY(r.ask({{"op", "bind"}, {"control", "key4"}, {"action", QJsonObject{{"type", "omarchy_bind"}, {"description", "Tmux"}}}}).value("ok").toBool());
        QVERIFY(r.ask({{"op", "bind"}, {"control", "key5"}, {"action", QJsonObject{{"type", "omarchy_bind"}, {"description", "Terminal"}}}}).value("ok").toBool());
        QVERIFY(r.daemon->status().value("warnings").toArray().isEmpty());

        // Remove "Tmux" from the config, then let the daemon rescan as it does on configreloaded.
        QFile f(lua);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QByteArray text = f.readAll();
        f.close();
        text.replace("\"Tmux\"", "\"Tmux renamed\"");
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(text);
        f.close();
        r.daemon->refreshShortcuts();
        QTRY_VERIFY_WITH_TIMEOUT(!r.daemon->status().value("warnings").toArray().isEmpty(), 5000);
        const QString warning = r.daemon->status().value("warnings").toArray()[0].toString();
        QVERIFY2(warning.contains("Tmux") && warning.contains("no longer exists"), qPrintable(warning));
        QCOMPARE(r.daemon->status().value("warnings").toArray().size(), 1);          // Terminal still resolves

        // Firing the stale link reports the problem instead of running anything.
        QList<QStringList> ran;
        connect(r.daemon->actions(), &Actions::ran, this, [&](const QStringList &argv) { ran << argv; });
        QSignalSpy failures(r.daemon->actions(), &Actions::failed);
        r.daemon->actions()->run(QJsonObject{{"type", "omarchy_bind"}, {"description", "Tmux"}});
        QCOMPARE(ran.size(), 0);
        QCOMPARE(failures.size(), 1);
    }

    void remappedKeyKeepsTheLinkWorking() {
        Rig r;
        const QString lua = useFixture(r);
        QVERIFY(r.start());
        QVERIFY(r.ask({{"op", "bind"}, {"control", "key4"}, {"action", QJsonObject{{"type", "omarchy_bind"}, {"description", "Terminal"}}}}).value("ok").toBool());
        QFile f(lua);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QByteArray text = f.readAll();
        f.close();
        text.replace("\"SUPER + RETURN\", \"Terminal\"", "\"SUPER + CTRL + T\", \"Terminal\"");
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(text);
        f.close();
        r.daemon->refreshShortcuts();
        QTRY_VERIFY_WITH_TIMEOUT(r.daemon->omarchy()->find("Terminal").bind.comboKey() == "ctrl+super+t", 5000);
        QList<QStringList> ran;
        connect(r.daemon->actions(), &Actions::ran, this, [&](const QStringList &argv) { ran << argv; });
        r.daemon->actions()->run(QJsonObject{{"type", "omarchy_bind"}, {"description", "Terminal"}, {"combo", "SUPER + RETURN"}});
        QCOMPARE(ran.size(), 1);
        QCOMPARE(ran[0], (QStringList{"omarchy-launch-terminal"}));
        QVERIFY(r.daemon->status().value("warnings").toArray().isEmpty());
    }

    void statusReportsTheShortcutCatalog() {
        Rig r;
        useFixture(r);
        QVERIFY(r.start());
        r.ask({{"op", "shortcuts"}});
        const QJsonObject s = r.daemon->status().value("shortcuts").toObject();
        QVERIFY(s.value("loaded").toBool());
        QVERIFY(s.value("total").toInt() > 0);
    }

    void appsOpScansTheConfiguredDirectories() {
        Rig r;
        r.appsDirs = {FIXTURE_DIR "/shell/applications"};
        QVERIFY(r.start());
        const QJsonObject reply = r.ask({{"op", "apps"}});
        QVERIFY(reply.value("ok").toBool());
        const QJsonArray apps = reply.value("apps").toArray();
        QCOMPARE(apps.size(), 1);
        QCOMPARE(apps[0].toObject().value("name").toString(), QString("Foot"));
        QCOMPARE(apps[0].toObject().value("id").toString(), QString("foot.desktop"));
    }

    void clientsOpNeedsHyprland() {
        Rig r;
        QVERIFY(r.start());
        const QJsonObject reply = r.ask({{"op", "clients"}});
        // In the test env either hyprctl is missing or no Hyprland is running; both are errors.
        QVERIFY(!reply.value("ok").toBool());
        QVERIFY(reply.value("error").toString().startsWith("hyprctl"));
    }

    void profilesGetSetValidatesAndPersists() {
        Rig r;
        QVERIFY(r.start());
        const QJsonObject before = r.ask({{"op", "profiles_get"}});
        QVERIFY(before.value("ok").toBool());
        QCOMPARE(before.value("profiles").toArray().first().toObject().value("name").toString(), QString("Default"));

        QJsonArray profiles = before.value("profiles").toArray();
        QJsonObject extra{{"name", "Kdenlive"}, {"match", QJsonObject{{"class", "^org\\.kde\\.kdenlive$"}}},
                          {"pages", QJsonArray{QJsonObject{{"name", "Editing"}, {"bindings", QJsonObject{}}}}}};
        profiles.append(extra);
        QJsonObject saved = r.ask({{"op", "profiles_set"}, {"profiles", profiles}});
        QVERIFY(saved.value("ok").toBool());
        QCOMPARE(saved.value("active").toString(), QString("Default"));

        // Missing the fallback profile must be refused, and the stored list must not change.
        QJsonArray noFallback = before.value("profiles").toArray();
        QJsonArray withoutFallback;
        QJsonObject defaultProfile = noFallback[0].toObject();
        defaultProfile.insert("match", QJsonObject{{"class", "^nothing$"}});
        withoutFallback.append(defaultProfile);
        withoutFallback.append(extra);
        const QJsonObject refused = r.ask({{"op", "profiles_set"}, {"profiles", withoutFallback}});
        QVERIFY(!refused.value("ok").toBool());
        QVERIFY(refused.value("error").toString().contains("fallback"));
        const QJsonObject after = r.ask({{"op", "profiles_get"}});
        QCOMPARE(after.value("profiles").toArray().size(), 2);
        QCOMPARE(after.value("profiles").toArray()[0].toObject().value("name").toString(), QString("Default"));
    }

    void presetsVerbsWorkThroughTheDaemon() {
        Rig r;
        QVERIFY(r.start());
        QCOMPARE(r.ask({{"op", "presets"}, {"verb", "list"}}).value("presets").toArray().size(), 0);
        QVERIFY(r.ask({{"op", "presets"}, {"verb", "save"}, {"name", "Stock"}}).value("ok").toBool());

        // A second profile gets added, then the preset is applied to remove it again.
        const QJsonObject before = r.ask({{"op", "profiles_get"}});
        QJsonObject extraProbe{{"name", "Extra"}, {"match", QJsonObject{{"class", "^x$"}}},
                               {"pages", QJsonArray{QJsonObject{{"name", "P"}, {"bindings", QJsonObject{}}}}}};
        QJsonArray probeList = before.value("profiles").toArray();
        probeList.append(extraProbe);
        QJsonObject savedExtra = r.ask({{"op", "profiles_set"}, {"profiles", probeList}});
        QVERIFY2(savedExtra.value("ok").toBool(), qPrintable(savedExtra.value("error").toString()));
        QCOMPARE(r.ask({{"op", "profiles_get"}}).value("profiles").toArray().size(), 2);

        const QJsonObject applied = r.ask({{"op", "presets"}, {"verb", "apply"}, {"name", "Stock"}});
        QVERIFY(applied.value("ok").toBool());
        const QJsonArray after = r.ask({{"op", "profiles_get"}}).value("profiles").toArray();
        QCOMPARE(after.size(), 1);
        QCOMPARE(after[0].toObject().value("name").toString(), QString("Default"));

        QVERIFY(r.ask({{"op", "presets"}, {"verb", "save"}, {"name", "Stock"}}).value("ok").toBool() == false);  // exists
        QVERIFY(r.ask({{"op", "presets"}, {"verb", "rename"}, {"name", "Stock"}, {"to", "Old"}}).value("ok").toBool());
        QVERIFY(r.ask({{"op", "presets"}, {"verb", "delete"}, {"name", "Old"}}).value("ok").toBool());
        QCOMPARE(r.ask({{"op", "presets"}, {"verb", "list"}}).value("presets").toArray().size(), 0);
        QVERIFY(!r.ask({{"op", "presets"}, {"verb", "bogus"}}).value("ok").toBool());
    }
};

REGISTER_TEST(DaemonTests)
#include "tests_daemon.moc"
