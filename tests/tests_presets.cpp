#include "testmain.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include "config.h"
#include "presets.h"

// Offscreen unit tests for the named profile snapshots.
class PresetsTests : public QObject {
    Q_OBJECT

private slots:
    void saveLoadRoundtrip();
    void refusals();
    void renameAndDelete();
    void brokenFileIsSkipped();

private:
    static QJsonArray profiles() {
        // The default config's own profile list; it is valid without running the daemon.
        QJsonObject doc = Config::defaults();
        return doc.value("profiles").toArray();
    }
};

void PresetsTests::saveLoadRoundtrip() {
    QTemporaryDir dir;
    PresetStore store(dir.path());
    QString error;
    QVERIFY(store.save("My Layout", profiles(), false, &error));
    QList<PresetStore::Info> list = store.list();
    QCOMPARE(list.size(), 1);
    QCOMPARE(list.first().name, "My Layout");
    QCOMPARE(list.first().profiles, profiles().size());
    QJsonArray loaded;
    QVERIFY(store.load("My Layout", &loaded, &error));
    QCOMPARE(loaded, profiles());
    QVERIFY(store.save("My Layout", profiles(), false, &error) == false);   // no overwrite
    QVERIFY(store.save("My Layout", profiles(), true, &error));             // overwrite
}

void PresetsTests::refusals() {
    QTemporaryDir dir;
    PresetStore store(dir.path());
    QString error;
    QVERIFY(store.save("", profiles(), false, &error) == false);
    QVERIFY(error.contains("name"));
    QVERIFY(store.save("   /// ", profiles(), false, &error) == false);
    QJsonArray bad;                                                      // profiles is invalid
    bad.append(QJsonObject{{"name", "Only"}, {"match", 0}, {"pages", QJsonArray{QJsonObject{{"name", "Page"}}}}});
    QVERIFY(store.save("Bad", bad, false, &error) == false);
    QVERIFY(error.contains("not a valid set of profiles"));
    QVERIFY(store.load("nothing", nullptr, &error) == false);
    QVERIFY(error.contains("nothing"));
}

void PresetsTests::renameAndDelete() {
    QTemporaryDir dir;
    PresetStore store(dir.path());
    QString error;
    QVERIFY(store.save("Before", profiles(), false, &error));
    QVERIFY(store.rename("Before", "After", &error));
    QJsonArray loaded;
    QVERIFY(store.load("After", &loaded, &error));       // the new name only
    QVERIFY(store.load("Before", nullptr, &error) == false);
    QVERIFY(store.remove("After", &error));
    QCOMPARE(store.list().size(), 0);
    QVERIFY(store.remove("After", &error) == false);
}

void PresetsTests::brokenFileIsSkipped() {
    QTemporaryDir dir;
    PresetStore store(dir.path());
    QVERIFY(store.save("Good", profiles(), false, nullptr));
    { QFile f(store.directory() + "/broken.json"); f.open(QIODevice::WriteOnly); f.write("{ nope"); }
    QStringList problems;
    QCOMPARE(store.list(&problems).size(), 1);
    QCOMPARE(problems.size(), 1);
}

REGISTER_TEST(PresetsTests)
#include "tests_presets.moc"
