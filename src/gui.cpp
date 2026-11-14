#include "gui.h"

#include "backend.h"
#include "theme.h"
#include "translator.h"
#include "version.h"

#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QUrl>

namespace Gui {

bool load(QQmlApplicationEngine *engine, Backend *backend, Theme *theme, Translator *translator) {
    QObject::connect(translator, &Translator::changed, engine, &QQmlEngine::retranslate);
    engine->rootContext()->setContextProperty("translator", translator);
    engine->rootContext()->setContextProperty("theme", theme);
    engine->rootContext()->setContextProperty("backend", backend);
    engine->load(QUrl("qrc:/Main.qml"));
    return !engine->rootObjects().isEmpty();
}

int run(const QString &socketPath) {
    QGuiApplication::setDesktopFileName("dialstudio");
    QGuiApplication::setWindowIcon(QIcon::fromTheme("dialstudio"));
    QQuickStyle::setStyle("Material");

    // Installed before anything is translated; every qsTr() binding re-evaluates when
    // the Omarchy language changes.
    Translator translator;
    QCoreApplication::installTranslator(&translator);
    Theme theme;
    Backend backend(socketPath);

    QQmlApplicationEngine engine;
    if (!Gui::load(&engine, &backend, &theme, &translator)) return 1;
    return QGuiApplication::exec();
}

} // namespace Gui
