// dialstudio: Ulanzi D100H controller for Omarchy.
//
// Every subcommand except `open` is headless. The offscreen platform is set before
// QGuiApplication exists so nothing needs a display; `open` leaves the platform alone.

#include <QGuiApplication>

#include "cli.h"
#include "version.h"

int main(int argc, char *argv[]) {
    const bool gui = argc > 1 && QByteArray(argv[1]) == "open";
    if (!gui) qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_FORCE_STDERR_LOGGING", "1");
    QGuiApplication app(argc, argv);
    app.setApplicationName("dialstudio");
    app.setApplicationVersion(DIALSTUDIO_VERSION);
    return Cli::run(app.arguments().mid(1));
}
