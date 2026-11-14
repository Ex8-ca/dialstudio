#include "appscan.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QSet>
#include <QStandardPaths>

QJsonObject AppEntry::toJson() const {
    return {{"id", id}, {"name", name}, {"generic_name", genericName}, {"comment", comment}, {"icon", icon},
            {"categories", QJsonArray::fromStringList(categories)}};
}

namespace AppScan {

namespace {

QString unescape(const QString &value) {
    if (!value.contains('\\')) return value;
    QString out;
    for (int i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 1 < value.size()) {
            const QChar next = value[++i];
            out += next == 's' ? QChar(' ') : next == 'n' ? QChar('\n') : next == 't' ? QChar('\t') : next;
        } else {
            out += value[i];
        }
    }
    return out;
}

QStringList splitList(const QString &value) {
    QStringList out;
    for (const QString &part : value.split(';', Qt::SkipEmptyParts)) out << part.trimmed();
    return out;
}

bool truthy(const QString &value) { return value.compare("true", Qt::CaseInsensitive) == 0 || value == "1"; }

} // namespace

QStringList defaultDirs() {
    QStringList dirs;
    auto add = [&](const QString &base) {
        const QString dir = QDir::cleanPath(base + "/applications");
        if (!base.isEmpty() && !dirs.contains(dir)) dirs << dir;
    };
    const QString home = qEnvironmentVariable("XDG_DATA_HOME");
    add(home.isEmpty() ? QDir::homePath() + "/.local/share" : home);
    // Flatpak's user exports are not always listed in XDG_DATA_DIRS, so they are added by hand.
    add(QDir::homePath() + "/.local/share/flatpak/exports/share");
    const QString data = qEnvironmentVariable("XDG_DATA_DIRS");
    for (const QString &base : (data.isEmpty() ? QStringLiteral("/usr/local/share:/usr/share") : data).split(':', Qt::SkipEmptyParts)) add(base);
    add("/var/lib/flatpak/exports/share");
    return dirs;
}

QStringList localeCandidates(const QString &locale) {
    QString rest = locale;
    const int dot = rest.indexOf('.');
    QString modifier;
    const int at = rest.indexOf('@');
    if (at >= 0) modifier = rest.mid(at);          // "@euro"
    if (dot >= 0) rest = rest.left(dot);
    else if (at >= 0) rest = rest.left(at);
    if (rest.isEmpty() || rest == "C" || rest == "POSIX") return {};
    QStringList out;
    const QString lang = rest.section('_', 0, 0);
    if (rest.contains('_')) {
        if (!modifier.isEmpty()) out << rest + modifier;
        out << rest;
    }
    if (!modifier.isEmpty()) out << lang + modifier;
    out << lang;
    return out;
}

bool parse(const QByteArray &bytes, const QString &id, const QStringList &locales, const QStringList &desktops,
           AppEntry *entry, bool *hidden) {
    if (hidden) *hidden = false;
    QHash<QString, QString> plain;
    QHash<QString, QHash<QString, QString>> localized;   // key -> locale -> value
    bool inEntry = false;
    for (QByteArray raw : bytes.split('\n')) {
        QString line = QString::fromUtf8(raw).trimmed();
        if (line.startsWith(QChar(0xFEFF))) line.remove(0, 1);
        if (line.isEmpty() || line.startsWith('#')) continue;
        if (line.startsWith('[')) { inEntry = line == "[Desktop Entry]"; continue; }
        if (!inEntry) continue;
        const int eq = line.indexOf('=');
        if (eq <= 0) continue;
        QString key = line.left(eq).trimmed();
        const QString value = line.mid(eq + 1).trimmed();
        const int bracket = key.indexOf('[');
        if (bracket > 0 && key.endsWith(']')) {
            localized[key.left(bracket)].insert(key.mid(bracket + 1, key.size() - bracket - 2), value);
        } else if (!plain.contains(key)) {
            plain.insert(key, value);
        }
    }
    auto text = [&](const QString &key) {
        for (const QString &locale : locales) {
            const QString value = localized.value(key).value(locale);
            if (!value.isEmpty()) return unescape(value);
        }
        return unescape(plain.value(key));
    };

    if (truthy(plain.value("Hidden"))) { if (hidden) *hidden = true; return false; }
    if (plain.value("Type") != "Application") return false;
    if (truthy(plain.value("NoDisplay"))) return false;
    if (plain.value("Exec").isEmpty() && plain.value("DBusActivatable") != "true") return false;
    const QStringList only = splitList(plain.value("OnlyShowIn")), not_ = splitList(plain.value("NotShowIn"));
    if (!only.isEmpty()) {
        bool any = false;
        for (const QString &d : desktops) any = any || only.contains(d, Qt::CaseInsensitive);
        if (!any) return false;
    }
    for (const QString &d : desktops)
        if (not_.contains(d, Qt::CaseInsensitive)) return false;
    const QString tryExec = plain.value("TryExec");
    if (!tryExec.isEmpty()) {
        const bool found = tryExec.startsWith('/') ? QFileInfo(tryExec).isExecutable() : !QStandardPaths::findExecutable(tryExec).isEmpty();
        if (!found) return false;
    }
    const QString name = text("Name");
    if (name.isEmpty()) return false;

    entry->id = id;
    entry->name = name;
    entry->genericName = text("GenericName");
    entry->comment = text("Comment");
    entry->icon = plain.value("Icon");
    entry->categories = splitList(plain.value("Categories"));
    return true;
}

QList<AppEntry> scan(const QStringList &dirs, const QStringList &locales, const QStringList &desktops) {
    QList<AppEntry> out;
    QSet<QString> seen;     // IDs already decided, shown or hidden: an earlier directory shadows later ones
    for (const QString &dir : dirs) {
        const QDir root(dir);
        if (!root.exists()) continue;
        QDirIterator it(dir, {"*.desktop"}, QDir::Files | QDir::Readable, QDirIterator::Subdirectories | QDirIterator::FollowSymlinks);
        QStringList files;
        while (it.hasNext()) files << it.next();
        files.sort();
        for (const QString &path : files) {
            QString id = root.relativeFilePath(path);
            id.replace('/', '-');
            if (seen.contains(id)) continue;
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) continue;
            AppEntry entry;
            bool hidden = false;
            const bool listed = parse(file.readAll(), id, locales, desktops, &entry, &hidden);
            seen.insert(id);
            if (!listed) continue;
            entry.path = path;
            out << entry;
        }
    }
    std::sort(out.begin(), out.end(), [](const AppEntry &a, const AppEntry &b) {
        const int c = a.name.compare(b.name, Qt::CaseInsensitive);
        return c != 0 ? c < 0 : a.id < b.id;
    });
    return out;
}

}
