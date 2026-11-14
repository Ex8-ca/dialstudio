// Drives the real window offscreen and saves screenshots with QQuickWindow::grabWindow().
//   DIALSTUDIO_SOCKET=<sock> dialstudio_shots <output-dir> [language-code]
// The daemon should run with DIALSTUDIO_DEV=1 so dial events can be injected through its
// own socket; they then travel the same path as real ones (daemon -> event stream -> window).
#include <QDir>
#include <QGuiApplication>
#include <QImage>
#include <QJsonObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QTest>

#include "backend.h"
#include "gui.h"
#include "ipc.h"
#include "theme.h"
#include "translator.h"

static QQuickWindow *g_window = nullptr;
static QString g_out;

static void shot(const QString &name) {
    QTest::qWait(250);
    const QImage image = g_window->grabWindow();
    const QString path = g_out + "/" + name + ".png";
    image.save(path);
    qInfo().noquote() << "saved" << path << image.size();
}

static QQuickItem *find(const QString &name) { return g_window->findChild<QQuickItem *>(name); }

static void click(QQuickItem *item) {
    const QPointF center = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
    QTest::mouseClick(g_window, Qt::LeftButton, Qt::NoModifier, center.toPoint());
    QTest::qWait(120);
}

// One request to the daemon from a separate connection, as the CLI would send it.
static QJsonObject ask(const QJsonObject &request) {
    IpcClient client;
    if (!client.connectTo(Ipc::socketPath(), 1000)) return {{"ok", false}};
    return client.request(request, 3000);
}

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    if (argc < 2) { qCritical() << "usage: dialstudio_shots <output-dir> [language]"; return 2; }
    g_out = argv[1];
    const QString language = argc > 2 ? argv[2] : "en";
    QDir().mkpath(g_out);
    QQuickStyle::setStyle("Material");

    // A throwaway theme directory (default accent) and language file, never the user's.
    QTemporaryDir scratch;
    QFile lang(scratch.filePath("language"));
    lang.open(QIODevice::WriteOnly);
    lang.write(language.toUtf8() + "\n");
    lang.close();
    Translator translator(scratch.filePath("language"));
    QCoreApplication::installTranslator(&translator);
    Theme theme(scratch.path());
    Backend backend;
    QQmlApplicationEngine engine;
    if (!Gui::load(&engine, &backend, &theme, &translator)) return 1;
    g_window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    g_window->resize(1180, 740);

    QTest::qWait(300);
    if (qEnvironmentVariable("DIALSTUDIO_SHOTS_MODE") == "banner") {
        // No daemon: the banner must show and nothing may be started behind the user's back.
        QTest::qWait(1500);
        qInfo() << "daemon" << backend.daemonRunning() << "connection" << backend.connection();
        shot("banner-no-daemon");
        click(find("key4"));
        QMetaObject::invokeMethod(find("inspector"), "chooseType", Q_ARG(QVariant, "volume"));
        QTest::qWait(300);
        qInfo() << "edit without a daemon:" << backend.message() << "undo" << backend.canUndo();
        shot("banner-edit-refused");
        return 0;
    }
    // Wait for the daemon and the dial.
    for (int i = 0; i < 100 && !(backend.daemonRunning() && !backend.shortcuts().isEmpty()); ++i) QTest::qWait(100);
    for (int i = 0; i < 60 && backend.connection() != "online" && backend.daemonRunning(); ++i) QTest::qWait(250);
    qInfo() << "daemon" << backend.daemonRunning() << "connection" << backend.connection() << "battery" << backend.battery()
            << "firmware" << backend.firmware() << "shortcuts" << backend.shortcuts().size();

    auto root = g_window->contentItem();
    auto inspector = find("inspector");
    shot("01-overview");

    // Select key4 through a real mouse click on its drawing.
    click(find("key4"));
    shot("02-key4-selected");

    // Keyboard shortcut: choose the type, press Record, press Ctrl+Shift+Z on the keyboard.
    click(find("key6"));
    QMetaObject::invokeMethod(inspector, "chooseType", Q_ARG(QVariant, "keys"));
    QTest::qWait(200);
    click(find("recordButton"));
    QTest::keyPress(g_window, Qt::Key_Control);
    QTest::keyPress(g_window, Qt::Key_Shift, Qt::ControlModifier | Qt::ShiftModifier);
    shot("03-recording-modifiers");
    QTest::keyClick(g_window, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    QTest::keyRelease(g_window, Qt::Key_Shift);
    QTest::keyRelease(g_window, Qt::Key_Control);
    QTest::qWait(500);
    qInfo() << "key6 after recording:" << backend.slotAction("key6", "press");
    shot("04-recorded");

    // Undo and redo.
    QTest::keyClick(g_window, Qt::Key_Z, Qt::ControlModifier);
    QTest::qWait(400);
    qInfo() << "after undo:" << backend.slotAction("key6", "press") << "canUndo" << backend.canUndo() << "canRedo" << backend.canRedo()
            << "focus item" << g_window->activeFocusItem();
    QTest::keyClick(g_window, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    QTest::qWait(400);
    qInfo() << "after redo:" << backend.slotAction("key6", "press") << "canUndo" << backend.canUndo() << "canRedo" << backend.canRedo();

    // Omarchy shortcut picker.
    click(find("key4"));
    QMetaObject::invokeMethod(inspector, "chooseType", Q_ARG(QVariant, "omarchy_bind"));
    QTest::qWait(300);
    shot("05-omarchy-picker");

    // Search for something that can't be run, to see the dimmed rows.
    if (auto search = g_window->findChild<QQuickItem *>("shortcutSearch")) {
        search->setProperty("text", "copy");
        shot("05b-omarchy-picker-unrunnable");
        search->setProperty("text", "terminal");
        QTest::qWait(200);
        shot("05c-omarchy-picker-terminal");
    }

    // Live events: a held key, then a knob turn, through the daemon's event stream. Another
    // key is selected, so the only reason key 2 can be lit is the event from the daemon.
    click(find("key1"));
    shot("05d-before-live-event");
    ask({{"op", "inject"}, {"kind", "down"}, {"key", 2}});
    QTest::qWait(150);
    shot("06-live-key2-held");
    ask({{"op", "inject"}, {"kind", "up"}, {"key", 2}});
    ask({{"op", "inject"}, {"kind", "rotate"}, {"direction", "cw"}, {"steps", 3}});
    QTest::qWait(250);
    shot("07-live-knob-turned");
    ask({{"op", "inject"}, {"kind", "click"}});
    QTest::qWait(60);
    shot("08-live-knob-click");

    // Test button: fires an action through the daemon (a harmless touch on a scratch file).
    const QString marker = g_out + "/test-fired.txt";
    QFile::remove(marker);
    QSignalSpy finished(&backend, &Backend::testFinished);
    backend.testAction({{"type", "exec"}, {"command", "touch " + marker}});
    finished.wait(3000);
    QTest::qWait(800);
    qInfo() << "test fired:" << (finished.isEmpty() ? QVariant("no reply") : finished[0][1]) << "file exists" << QFile::exists(marker);

    // Knob inspector, and the help overlay.
    click(find("key1"));
    g_window->findChild<QQuickItem *>("window");
    QMetaObject::invokeMethod(g_window, "select", Q_ARG(QVariant, "knob"), Q_ARG(QVariant, "rotate_cw"));
    QTest::qWait(300);
    shot("09-knob-selected");
    QTest::keyClick(g_window, Qt::Key_Question, Qt::ShiftModifier);
    QTest::qWait(250);
    shot("10-help");
    return 0;
}
