// dialstudio: Ulanzi D100H controller for Omarchy.
//
// Every subcommand in this version is headless. The offscreen platform is set
// before QGuiApplication exists so nothing ever needs a display; the GUI
// (`dialstudio open`) will switch it off again when it arrives.

#include <QGuiApplication>

#include "cli.h"
#include "version.h"

int main(int argc, char *argv[]) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_FORCE_STDERR_LOGGING", "1");
    QGuiApplication app(argc, argv);
    app.setApplicationName("dialstudio");
    app.setApplicationVersion(DIALSTUDIO_VERSION);
    return Cli::run(app.arguments().mid(1));
}
