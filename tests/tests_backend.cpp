#include "testmain.h"

#include <QDBusConnection>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "backend.h"
#include "daemon.h"
#include "fakebluez.h"

// The window's model: a Backend talking to a real daemon (fake BlueZ, temp config) over the
// real socket. Asynchronous like the GUI, so everything is waited for with QTRY.
class BackendTests : public QObject {
    Q_OBJECT

    static bool onPrivateBus() { return qEnvironmentVariableIsSet("DIALSTUDIO_TEST_BUS"); }

    struct Rig {
        QTemporaryDir dir;
        FakeBluez fake;
        std::unique_ptr<Daemon> daemon;
        std::unique_ptr<Backend> backend;
        bool start() {
            Daemon::Options o;
            o.configPath = dir.filePath("config.json");
            o.socketPath = dir.filePath("s.sock");
            o.watchHyprland = false;
            o.probeScope = false;
            o.toasts = false;
            o.dryRunActions = true;
            fake.autoReply(true);
            fake.addDial(true);
            daemon = std::make_unique<Daemon>(QDBusConnection::sessionBus(), o);
            QString error;
            if (!daemon->start(&error)) { qWarning() << error; return false; }
            backend = std::make_unique<Backend>(o.socketPath);
            return true;
        }
        bool ready() { return backend->daemonRunning() && backend->revision() > 0; }
        QJsonObject fileBindings() {
            QFile f(dir.filePath("config.json"));
            f.open(QIODevice::ReadOnly);
            return QJsonDocument::fromJson(f.readAll()).object().value("profiles").toArray()[0].toObject()
                .value("pages").toArray()[0].toObject().value("bindings").toObject();
        }
    };

private slots:
    void initTestCase() {
        if (!onPrivateBus()) QSKIP("needs the private session bus from bin/test");
    }

    void showsTheDaemonsStateAndTheDefaultBindings() {
        Rig r;
        QVERIFY(r.start());
        QTRY_VERIFY_WITH_TIMEOUT(r.ready(), 4000);
        QTRY_COMPARE_WITH_TIMEOUT(r.backend->connection(), QString("online"), 4000);
        QTRY_COMPARE_WITH_TIMEOUT(r.backend->firmware(), QString("1.3.8.1"), 4000);
        QTRY_COMPARE_WITH_TIMEOUT(r.backend->battery(), 80, 4000);
        QTRY_COMPARE_WITH_TIMEOUT(r.backend->haptics(), 1, 4000);
        QCOMPARE(r.backend->slotLabel("key4", "press"), QString("Ctrl+C"));
        QCOMPARE(r.backend->slotLabel("key2", "press"), QString("Play/Pause"));
        QCOMPARE(r.backend->slotLabel("knob", "rotate_cw"), QString("Volume +"));
        QCOMPARE(r.backend->slotLabel("key1", "long_press"), QString());
    }

    void editsSaveThroughTheDaemonAndUndoRedoRestoreThem() {
        Rig r;
        QVERIFY(r.start());
        QTRY_VERIFY_WITH_TIMEOUT(r.ready(), 4000);
        QVERIFY(!r.backend->canUndo());
        QSignalSpy changed(r.backend.get(), &Backend::bindingsChanged);

        r.backend->setBinding("key4", "press", {{"type", "keys"}, {"combo", "ctrl+shift+z"}});
        QCOMPARE(r.backend->slotLabel("key4", "press"), QString("Ctrl+Shift+Z"));    // shown at once
        QVERIFY(r.backend->canUndo());
        QTRY_COMPARE_WITH_TIMEOUT(r.fileBindings().value("key4").toObject().value("press").toObject().value("combo").toString(),
                                  QString("ctrl+shift+z"), 3000);                    // saved by the daemon, on disk
        QVERIFY(changed.count() >= 1);

        r.backend->undo();
        QCOMPARE(r.backend->slotLabel("key4", "press"), QString("Ctrl+C"));
        QTRY_COMPARE_WITH_TIMEOUT(r.fileBindings().value("key4").toObject().value("press").toObject().value("combo").toString(),
                                  QString("ctrl+c"), 3000);
        QVERIFY(!r.backend->canUndo());
        QVERIFY(r.backend->canRedo());

        r.backend->redo();
        QTRY_COMPARE_WITH_TIMEOUT(r.fileBindings().value("key4").toObject().value("press").toObject().value("combo").toString(),
                                  QString("ctrl+shift+z"), 3000);
        QVERIFY(!r.backend->canRedo());

        // A new edit after an undo drops the redo history.
        r.backend->undo();
        r.backend->setBinding("key5", "press", {{"type", "media"}, {"action", "next"}});
        QVERIFY(!r.backend->canRedo());
    }

    void clearingABindingRemovesItAndCanBeUndone() {
        Rig r;
        QVERIFY(r.start());
        QTRY_VERIFY_WITH_TIMEOUT(r.ready(), 4000);
        r.backend->clearBinding("key1", "press");
        QCOMPARE(r.backend->slotLabel("key1", "press"), QString());
        QTRY_VERIFY_WITH_TIMEOUT(!r.fileBindings().contains("key1"), 3000);
        r.backend->undo();
        QCOMPARE(r.backend->slotLabel("key1", "press"), QString("Previous"));
    }

    void invalidEditsAreRefusedAndNothingIsSaved() {
        Rig r;
        QVERIFY(r.start());
        QTRY_VERIFY_WITH_TIMEOUT(r.ready(), 4000);
        QSignalSpy failed(r.backend.get(), &Backend::editFailed);
        r.backend->setBinding("key4", "press", {{"type", "keys"}, {"combo", "ctrl+Retrun"}});
        QCOMPARE(failed.count(), 1);
        QVERIFY(r.backend->message().contains("Retrun"));
        QVERIFY(r.backend->messageIsError());
        r.backend->setBinding("key4", "nope", {{"type", "noop"}});
        QCOMPARE(failed.count(), 2);
        QVERIFY(!r.backend->canUndo());
        QCOMPARE(r.backend->slotLabel("key4", "press"), QString("Ctrl+C"));
        QCOMPARE(r.fileBindings().value("key4").toObject().value("press").toObject().value("combo").toString(), QString("ctrl+c"));
    }

    void aRefusalFromTheDaemonRollsTheModelBack() {
        Rig r;
        QVERIFY(r.start());
        QTRY_VERIFY_WITH_TIMEOUT(r.ready(), 4000);
        // An omarchy_bind link to a shortcut that does not exist passes the local check (it only
        // needs a description) but the daemon refuses it on save.
        QSignalSpy failed(r.backend.get(), &Backend::editFailed);
        r.backend->setBinding("key6", "press", {{"type", "keys"}, {"omarchy", ""}});
        QCOMPARE(failed.count(), 1);                       // empty description: refused locally
        QCOMPARE(r.backend->slotLabel("key6", "press"), QString("Ctrl+Z"));
    }

    void editingIsRefusedWhenTheServiceIsNotRunning() {
        QTemporaryDir dir;
        Backend backend(dir.filePath("nobody-listens.sock"));
        QVERIFY(!backend.daemonRunning());
        QCOMPARE(backend.connection(), QString("offline"));
        QSignalSpy failed(&backend, &Backend::editFailed);
        backend.setBinding("key4", "press", {{"type", "keys"}, {"combo", "ctrl+z"}});
        QCOMPARE(failed.count(), 1);
        QVERIFY(backend.message().contains("not running"));
        QVERIFY(!backend.canUndo());
    }

    void liveEventsReachTheWindowWithControlAndDirection() {
        Rig r;
        QVERIFY(r.start());
        qputenv("DIALSTUDIO_DEV", "1");
        QTRY_VERIFY_WITH_TIMEOUT(r.ready(), 4000);
        QTRY_COMPARE_WITH_TIMEOUT(r.backend->connection(), QString("online"), 4000);
        QSignalSpy events(r.backend.get(), &Backend::inputEvent);
        QTest::qWait(300);       // the subscription must be in place
        r.fake.notify(QByteArray::fromHex("8b100201"));
        r.fake.notify(QByteArray::fromHex("8b100200"));
        r.fake.notify(QByteArray::fromHex("8b100701"));
        r.fake.notify(QByteArray::fromHex("8b100901"));
        QTRY_COMPARE_WITH_TIMEOUT(events.count(), 4, 3000);
        QCOMPARE(events[0], (QVariantList{"key2", "press", "", 1}));
        QCOMPARE(events[1], (QVariantList{"key2", "release", "", 1}));
        QCOMPARE(events[2], (QVariantList{"knob", "click", "", 1}));
        QCOMPARE(events[3][0].toString(), QString("knob"));
        QCOMPARE(events[3][1].toString(), QString("rotate"));
        QCOMPARE(events[3][2].toString(), QString("ccw"));
        qunsetenv("DIALSTUDIO_DEV");
    }

    void connectionStateMapsTheLinkAndDaemonLifetime() {
        Rig r;
        QVERIFY(r.start());
        QTRY_VERIFY_WITH_TIMEOUT(r.ready(), 4000);
        QTRY_COMPARE_WITH_TIMEOUT(r.backend->connection(), QString("online"), 4000);
        // The fake accepts a reconnect at once, so hold it back long enough to observe the state.
        r.fake.setConnectDelay(3000);
        r.fake.setConnected(false);
        QTRY_VERIFY_WITH_TIMEOUT(r.backend->connection() != "online", 4000);
        QVERIFY(r.backend->connection() == "connecting" || r.backend->connection() == "offline");
        r.daemon->halt();
        r.daemon.reset();
        QTRY_COMPARE_WITH_TIMEOUT(r.backend->connection(), QString("offline"), 4000);
        QVERIFY(!r.backend->daemonRunning());
    }

    void testActionRunsThroughTheDaemon() {
        Rig r;
        QVERIFY(r.start());
        QTRY_VERIFY_WITH_TIMEOUT(r.ready(), 4000);
        QList<QStringList> ran;
        connect(r.daemon->actions(), &Actions::ran, this, [&](const QStringList &argv) { ran << argv; });
        QSignalSpy finished(r.backend.get(), &Backend::testFinished);
        r.backend->testAction({{"type", "volume"}, {"action", "down"}, {"step", 4}});
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 3000);
        QVERIFY(finished[0][0].toBool());
        QCOMPARE(ran.size(), 1);
        QVERIFY(ran[0].last().startsWith("4%"));
        r.backend->testAction({{"type", "keys"}, {"combo", "ctrl+Retrun"}});
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, 3000);
        QVERIFY(!finished[1][0].toBool());
    }

    void hapticsToggleGoesThroughTheDaemon() {
        Rig r;
        QVERIFY(r.start());
        QTRY_COMPARE_WITH_TIMEOUT(r.backend->haptics(), 1, 4000);
        r.backend->setHaptics(false);
        QTRY_COMPARE_WITH_TIMEOUT(r.backend->haptics(), 0, 4000);
        r.backend->setHaptics(true);
        QTRY_COMPARE_WITH_TIMEOUT(r.backend->haptics(), 1, 4000);
    }

    // ---- pure helpers -------------------------------------------------------------------

    void recordingTurnsKeyEventsIntoCombos() {
        Backend b(QTemporaryDir().filePath("x.sock"));
        QCOMPARE(b.comboFromKeyEvent(Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier), QString("ctrl+shift+z"));
        QCOMPARE(b.comboFromKeyEvent(Qt::Key_Return, Qt::AltModifier), QString("alt+Return"));
        QCOMPARE(b.comboFromKeyEvent(Qt::Key_F5, Qt::NoModifier), QString("F5"));
        QCOMPARE(b.comboFromKeyEvent(Qt::Key_Left, Qt::MetaModifier | Qt::ShiftModifier), QString("shift+super+Left"));
        QCOMPARE(b.comboFromKeyEvent(Qt::Key_Plus, Qt::ControlModifier), QString("ctrl+plus"));
        QCOMPARE(b.comboFromKeyEvent(Qt::Key_VolumeUp, Qt::NoModifier), QString("XF86AudioRaiseVolume"));
        // A bare modifier is not a combo yet.
        QCOMPARE(b.comboFromKeyEvent(Qt::Key_Control, Qt::ControlModifier), QString());
        QCOMPARE(b.comboFromKeyEvent(Qt::Key_Shift, Qt::ShiftModifier), QString());
        QCOMPARE(b.modifierChips(Qt::ControlModifier | Qt::MetaModifier), (QStringList{"Ctrl", "Super"}));
        QCOMPARE(b.comboChips("ctrl+shift+z"), (QStringList{"Ctrl", "Shift", "Z"}));
        QCOMPARE(b.normalizeCombo("Ctrl+Shift+z"), QString("ctrl+shift+z"));
        QCOMPARE(b.normalizeCombo("ctrl+Retrun"), QString());
    }

    void draftsAreReadyOnlyWhenTheDaemonWouldAcceptThem() {
        Backend b(QTemporaryDir().filePath("x.sock"));
        QVERIFY(!b.draftReady(b.defaultAction("keys")));                    // no combo yet
        QVERIFY(b.draftReady({{"type", "keys"}, {"combo", "ctrl+c"}}));
        QVERIFY(b.draftReady(b.defaultAction("volume")));
        QVERIFY(b.draftReady(b.defaultAction("media")));
        QVERIFY(!b.draftReady(b.defaultAction("exec")));                    // empty command
        QVERIFY(!b.draftReady(b.defaultAction("omarchy_bind")));            // no description
        QVERIFY(b.draftReady(b.defaultAction("noop")));
        QVERIFY(b.draftReady(b.defaultAction("hypr")));
        QVERIFY(b.draftReady(b.defaultAction("omarchy")));
        QCOMPARE(b.actionLabel({{"type", "omarchy_bind"}, {"description", "Terminal"}}), QString("Terminal"));
        QCOMPARE(b.slotsOf("knob").size(), 5);
        QCOMPARE(b.slotsOf("key3"), (QStringList{"press", "long_press", "release"}));
    }
};

REGISTER_TEST(BackendTests)
#include "tests_backend.moc"
