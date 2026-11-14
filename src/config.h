// JSON config: defaults, validation, atomic save, hot reload, binding edits.
#pragma once

#include <QByteArray>
#include <QFileSystemWatcher>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QTimer>

struct ControlRef {
    QString control;   // key1..key7 or knob
    QString slot;      // press, long_press, release, click, rotate_cw, ...
};

class Config : public QObject {
    Q_OBJECT
public:
    explicit Config(const QString &path, QObject *parent = nullptr);

    static QString defaultPath();
    static QJsonObject defaults();
    // Unknown versions are rejected; a missing version is treated as 1.
    static bool validate(const QJsonObject &doc, QStringList *errors);
    static const QStringList &keyControls();
    // "key4" -> key4/press (or onSlot), "knob.rotate_cw" -> knob/rotate_cw, "knob.rotate_cw@key4".
    static bool parseControl(const QString &text, const QString &on, ControlRef *out, QString *error);
    static bool validSlot(const QString &control, const QString &slot, QString *error);

    // Loads the file, creating the defaults if it does not exist (never overwrites).
    bool load(QString *error);
    // Writes atomically (temp file + rename) and updates the in-memory document.
    bool save(QString *error);
    // Re-reads the file; a broken file keeps the previous document and emits loadFailed.
    void reload();
    void setWatching(bool on);

    const QJsonObject &doc() const { return m_doc; }
    QString path() const { return m_path; }
    QJsonObject device() const { return m_doc.value("device").toObject(); }
    bool osdEnabled() const { return m_doc.value("osd").toObject().value("enabled").toBool(true); }
    QJsonArray profiles() const { return m_doc.value("profiles").toArray(); }
    int profileIndex(const QString &name) const;
    QString defaultProfileName() const;

    // The profile-list half of validate(), so it can be used for presets and drafts.
    static bool validateProfiles(const QJsonArray &profiles, QStringList *problems);

    // Edits. All validate first, then save, then emit changed().
    bool bind(const QString &profile, int page, const ControlRef &ref, const QJsonObject &action, QString *error);
    bool unbind(const QString &profile, int page, const ControlRef &ref, bool allSlots, QString *error);
    bool setDevice(const QString &key, const QJsonValue &value, QString *error);
    bool replace(const QJsonObject &doc, QString *error);

signals:
    void changed();
    void loadFailed(const QString &message);

private:
    bool parseBytes(const QByteArray &bytes, QJsonObject *doc, QString *error) const;
    void arm();

    QString m_path;
    QJsonObject m_doc;
    QByteArray m_bytes;       // what we last read or wrote, to ignore our own saves
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    bool m_watching = false;
};
