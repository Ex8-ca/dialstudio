#include <QCoreApplication>

#include "testmain.h"

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    int status = 0;
    // DIALSTUDIO_TEST_CLASS=BluezLinkTests runs a single class while debugging.
    const QByteArray only = qgetenv("DIALSTUDIO_TEST_CLASS");
    for (const auto &entry : TestRegistry::all())
        if (only.isEmpty() || only == entry.first) status |= entry.second(argc, argv);
    return status;
}
