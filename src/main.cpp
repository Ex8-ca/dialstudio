// dialstudio: Ulanzi D100H controller for Omarchy.
#include <QCoreApplication>
#include <QTextStream>

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    QTextStream(stdout) << "dialstudio " DIALSTUDIO_VERSION "\n";
    return 0;
}
