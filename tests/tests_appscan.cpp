#include "testmain.h"

#include <QTemporaryDir>
#include <QtTest>

#include "appscan.h"

// Offscreen unit tests for the .desktop scanner that feeds the app picker.
class AppscanTests : public QObject {
    Q_OBJECT

private slots:
    void localeOrder();
    void rejectsHiddenAndNoDisplay();
    void firstDirectoryWins();
    void localizedNames();

private:
    static void writeFile(const QString &path, const QByteArray &bytes) {
        QFile f(path);
        f.open(QIODevice::WriteOnly);
        f.write(bytes);
    }
};

static const QByteArray kEntry = "[Desktop Entry]\n"
                                 "Type=Application\n"
                                 "Name=%1\n"
                                 "Name[da]=Terminal\n"
                                 "Comment=Bare en terminal\n"
                                 "Exec=foot\n"
                                 "__END__";

void AppscanTests::localeOrder() {
    using namespace AppScan;
    const QStringList dk = AppScan::localeCandidates("da_DK.UTF-8@euro");
    QVERIFY2(dk == (QStringList{"da_DK@euro"} << "da_DK" << "da@euro" << "da"), qPrintable(dk.join(',')));
    const QStringList us = AppScan::localeCandidates("en_US.UTF-8");
    QVERIFY(us == (QStringList{"en_US"} << "en"));
    QVERIFY(AppScan::localeCandidates("C").isEmpty());
    QVERIFY(AppScan::localeCandidates("de") == QStringList{"de"});
}

void AppscanTests::rejectsHiddenAndNoDisplay() {
    QTemporaryDir dir;
    QVERIFY(QDir().mkpath(dir.path() + "/applications"));
    writeFile(dir.path() + "/applications/plain.desktop",
              QByteArray(kEntry).replace("%1", "Foot"));
    writeFile(dir.path() + "/applications/hidden.desktop",
              QByteArray(kEntry).replace("%1", "Hidden").replace("Exec=foot",
                                                                 "Exec=foot\nHidden=true"));
    writeFile(dir.path() + "/applications/nodisplay.desktop",
              QByteArray(kEntry).replace("%1", "Unlisted").replace("Exec=foot",
                                                                   "Exec=foot\nNoDisplay=true"));
    writeFile(dir.path() + "/applications/notapp.desktop",
              QByteArray(kEntry).replace("%1", "Link").replace("Type=Application", "Type=Link"));
    const QList<AppEntry> apps = AppScan::scan({dir.path() + "/applications"}, {"en"}, {"GNOME"});
    QCOMPARE(apps.size(), 1);
    QCOMPARE(apps.first().name, "Foot");
    QCOMPARE(apps.first().id, "plain.desktop");
}

void AppscanTests::firstDirectoryWins() {
    QTemporaryDir dir;
    QVERIFY(QDir().mkpath(dir.path() + "/a/applications"));
    QVERIFY(QDir().mkpath(dir.path() + "/b/applications"));
    writeFile(dir.path() + "/a/applications/x.desktop", QByteArray(kEntry).replace("%1", "From A"));
    writeFile(dir.path() + "/b/applications/x.desktop", QByteArray(kEntry).replace("%1", "From B"));
    writeFile(dir.path() + "/b/applications/y.desktop", QByteArray(kEntry).replace("%1", "Only B"));
    const QList<AppEntry> apps = AppScan::scan({dir.path() + "/a/applications", dir.path() + "/b/applications"},
                                               {"en"}, {"GNOME"});
    QCOMPARE(apps.size(), 2);
    QHash<QString, QString> name;
    for (const AppEntry &e : apps) name[e.id] = e.name;
    QCOMPARE(name["x.desktop"], "From A");
    QCOMPARE(name["y.desktop"], "Only B");
}

void AppscanTests::localizedNames() {
    QTemporaryDir dir;
    QVERIFY(QDir().mkpath(dir.path() + "/applications"));
    writeFile(dir.path() + "/applications/t.desktop", QByteArray(kEntry).replace("%1", "Terminal"));
    const QList<AppEntry> da = AppScan::scan({dir.path() + "/applications"}, {"da_DK", "da"}, {"GNOME"});
    QCOMPARE(da.first().name, "Terminal");        // the Name[da] translation
    QCOMPARE(da.first().comment, "Bare en terminal");
    const QList<AppEntry> en = AppScan::scan({dir.path() + "/applications"}, {"en"}, {"GNOME"});
    QCOMPARE(en.first().name, "Terminal");
}

REGISTER_TEST(AppscanTests)
#include "tests_appscan.moc"
