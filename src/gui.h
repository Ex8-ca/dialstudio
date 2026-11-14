// The window: `dialstudio open`. A client of the daemon; it never starts or replaces it.
#pragma once

#include <QString>

class QQmlApplicationEngine;
class Backend;
class Theme;
class Translator;

namespace Gui {
// Registers the context objects the QML expects and loads qrc:/Main.qml.
bool load(QQmlApplicationEngine *engine, Backend *backend, Theme *theme, Translator *translator);
int run(const QString &socketPath);
}
