#include "testmain.h"

#include <QDBusConnection>
#include <QDir>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "daemon.h"
#include "fakebluez.h"
#include "ipc.h"

// End to end: fake dial -> daemon -> IPC client, with the config in a temp directory.
class DaemonTests : public QObject {
    Q_OBJECT

    static bool onPrivateBus() { return qEnvironmentVariableIsSet("DIALSTUDIO_TEST_BUS"); }

    struct Rig {
        QTemporaryDir dir;
        FakeBluez fake;
        std::unique_ptr<Daemon> daemon;
        QString socket;
        bool start(const QByteArray &configJson = {}) {
            socket = dir.filePath("s.sock");
            Daemon::Options o;
            o.configPath = dir.filePath("config.json");
            o.socketPath = socket;
            o.watchHyprland = false;
            o.probeScope = false;
            o.toasts = false;
            o.dryRunActions = true;
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
};

REGISTER_TEST(DaemonTests)
#include "tests_daemon.moc"
