// Named snapshots of the whole profile list, kept as one JSON file each in
// ~/.config/dialstudio/presets/. Saving, listing, renaming and deleting only touch that
// directory; making a preset active is a normal validated config save done by the daemon.
#pragma once

#include <QJsonArray>
#include <QList>
#include <QString>
#include <QStringList>

class PresetStore {
public:
    struct Info {
        QString name;
        QString file;       // base name without .json
        int profiles = 0;
        QString saved;      // ISO 8601, UTC
    };

    explicit PresetStore(const QString &directory) : m_dir(directory) {}

    QString directory() const { return m_dir; }
    // File-safe form of a name; names that differ only in case or punctuation share a file.
    static QString slug(const QString &name);
    static bool validName(const QString &name, QString *error);

    // Unreadable or invalid files are skipped and described in *problems.
    QList<Info> list(QStringList *problems = nullptr) const;
    bool save(const QString &name, const QJsonArray &profiles, bool overwrite, QString *error) const;
    bool load(const QString &name, QJsonArray *profiles, QString *error) const;
    bool remove(const QString &name, QString *error) const;
    bool rename(const QString &from, const QString &to, QString *error) const;

private:
    QString pathFor(const QString &name) const;
    bool write(const QString &path, const QString &name, const QJsonArray &profiles, QString *error) const;

    QString m_dir;
};
