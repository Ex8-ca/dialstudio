#include "presets.h"

#include "config.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace {

constexpr int MaxName = 80;

bool fail(QString *error, const QString &message) {
    if (error) *error = message;
    return false;
}

} // namespace

QString PresetStore::slug(const QString &name) {
    QString out;
    for (const QChar c : name.trimmed().toLower()) {
        if (c.isLetterOrNumber() || c == '_') out += c;
        else if (!out.isEmpty() && !out.endsWith('-')) out += '-';
    }
    while (out.endsWith('-')) out.chop(1);
    return out;
}

bool PresetStore::validName(const QString &name, QString *error) {
    if (name.trimmed().isEmpty()) return fail(error, "a preset needs a name");
    if (name.trimmed().size() > MaxName) return fail(error, QStringLiteral("a preset name can have at most %1 characters").arg(MaxName));
    if (slug(name).isEmpty()) return fail(error, "a preset name needs at least one letter or digit");
    return true;
}

QString PresetStore::pathFor(const QString &name) const { return m_dir + "/" + slug(name) + ".json"; }

bool PresetStore::write(const QString &path, const QString &name, const QJsonArray &profiles, QString *error) const {
    if (!QDir().mkpath(m_dir)) return fail(error, "cannot create " + m_dir);
    const QJsonObject doc{{"dialstudio_preset", 1}, {"name", name.trimmed()},
                          {"saved", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}, {"profiles", profiles}};
    const QByteArray bytes = QJsonDocument(doc).toJson(QJsonDocument::Indented);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        return fail(error, QStringLiteral("cannot write %1: %2").arg(path, file.errorString()));
    return true;
}

bool PresetStore::save(const QString &name, const QJsonArray &profiles, bool overwrite, QString *error) const {
    if (!validName(name, error)) return false;
    QStringList problems;
    if (!Config::validateProfiles(profiles, &problems)) return fail(error, "not a valid set of profiles: " + problems.join("; "));
    const QString path = pathFor(name);
    if (!overwrite && QFileInfo::exists(path)) return fail(error, QStringLiteral("a preset named '%1' already exists").arg(name.trimmed()));
    return write(path, name, profiles, error);
}

bool PresetStore::load(const QString &name, QJsonArray *profiles, QString *error) const {
    if (slug(name).isEmpty()) return fail(error, "a preset needs a name");
    QFile file(pathFor(name));
    if (!file.open(QIODevice::ReadOnly)) return fail(error, QStringLiteral("no preset named '%1'").arg(name));
    QJsonParseError parse;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parse);
    if (!doc.isObject()) return fail(error, QStringLiteral("preset '%1' is not valid JSON: %2").arg(name, parse.errorString()));
    const QJsonArray list = doc.object().value("profiles").toArray();
    QStringList problems;
    // A preset edited by hand can be wrong; loading must not hand the editor something the daemon refuses.
    if (!Config::validateProfiles(list, &problems)) return fail(error, QStringLiteral("preset '%1' is not valid: %2").arg(name, problems.join("; ")));
    *profiles = list;
    return true;
}

QList<PresetStore::Info> PresetStore::list(QStringList *problems) const {
    QList<Info> out;
    const QDir dir(m_dir);
    for (const QFileInfo &info : dir.entryInfoList({"*.json"}, QDir::Files, QDir::Name)) {
        QFile file(info.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly)) continue;
        const QJsonObject doc = QJsonDocument::fromJson(file.readAll()).object();
        const QString name = doc.value("name").toString();
        const QJsonArray profiles = doc.value("profiles").toArray();
        if (!doc.contains("dialstudio_preset") || name.isEmpty() || profiles.isEmpty()) {
            if (problems) *problems << QStringLiteral("%1 is not a Dial Studio preset").arg(info.fileName());
            continue;
        }
        out << Info{name, info.completeBaseName(), int(profiles.size()), doc.value("saved").toString()};
    }
    std::sort(out.begin(), out.end(), [](const Info &a, const Info &b) { return a.name.compare(b.name, Qt::CaseInsensitive) < 0; });
    return out;
}

bool PresetStore::remove(const QString &name, QString *error) const {
    const QString path = pathFor(name);
    if (slug(name).isEmpty() || !QFileInfo::exists(path)) return fail(error, QStringLiteral("no preset named '%1'").arg(name));
    if (!QFile::remove(path)) return fail(error, "cannot delete " + path);
    return true;
}

bool PresetStore::rename(const QString &from, const QString &to, QString *error) const {
    if (!validName(to, error)) return false;
    QJsonArray profiles;
    if (!load(from, &profiles, error)) return false;
    const QString source = pathFor(from), target = pathFor(to);
    // Renaming "Video" to "video" is the same file and only changes the stored name.
    if (source != target && QFileInfo::exists(target)) return fail(error, QStringLiteral("a preset named '%1' already exists").arg(to.trimmed()));
    if (!write(target, to, profiles, error)) return false;
    if (source != target) QFile::remove(source);
    return true;
}
