// Lists the applications a user can launch from a key: the visible .desktop entries of the XDG
// application directories (and Flatpak's exports), with their localized names. The stable
// .desktop ID is what gets stored in the config, never the Exec line.
#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

struct AppEntry {
    QString id;            // "org.gnome.Nautilus.desktop", the path below applications/ with '/' as '-'
    QString name;          // localized
    QString genericName;
    QString comment;
    QString icon;          // theme icon name or absolute path, resolved at paint time
    QStringList categories;
    QString path;

    QJsonObject toJson() const;
};

namespace AppScan {

// $XDG_DATA_HOME, $XDG_DATA_DIRS and the Flatpak export directories, each + "/applications",
// in lookup order (the user's own directory first, so it overrides the system's).
QStringList defaultDirs();

// "da_DK.UTF-8@euro" -> ["da_DK@euro", "da_DK", "da@euro", "da"], the lookup order of the spec.
QStringList localeCandidates(const QString &locale);

// Parses one entry. Returns false for anything that must not be listed (not an application,
// NoDisplay, Hidden, a failing TryExec, or excluded for these desktops). *hidden is set when the
// file asks to be hidden, which also hides a same-named entry in a later directory.
bool parse(const QByteArray &bytes, const QString &id, const QStringList &locales, const QStringList &desktops,
           AppEntry *entry, bool *hidden = nullptr);

// Scans the directories in order; the first directory that has an ID wins. Sorted by name.
QList<AppEntry> scan(const QStringList &dirs, const QStringList &locales, const QStringList &desktops);

}
