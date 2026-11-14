#include "config.h"

#include "actions.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

namespace {

QJsonObject media(const char *action) { return {{"type", "media"}, {"action", action}}; }
QJsonObject keys(const char *combo) { return {{"type", "keys"}, {"combo", combo}}; }
QJsonObject volume(const char *action, int step = 2) {
    QJsonObject o{{"type", "volume"}, {"action", action}};
    if (step) o.insert("step", step);
    return o;
}

const QStringList KeySlots = {"press", "long_press", "release"};
const QStringList KnobSlots = {"click", "rotate_cw", "rotate_ccw", "press_rotate_cw", "press_rotate_ccw"};

QString join(const QString &a, const QString &b) { return a + "." + b; }

} // namespace

const QStringList &Config::keyControls() {
    static const QStringList list = {"key1", "key2", "key3", "key4", "key5", "key6", "key7"};
    return list;
}

QString Config::defaultPath() {
    const QString base = qEnvironmentVariableIsSet("XDG_CONFIG_HOME") && !qEnvironmentVariable("XDG_CONFIG_HOME").isEmpty()
                             ? qEnvironmentVariable("XDG_CONFIG_HOME") : QDir::homePath() + "/.config";
    return base + "/dialstudio/config.json";
}

// The factory behaviour of the dial: media keys on the top row, copy/paste/undo/redo
// on the side keys, volume on the knob. Nothing changes until the user edits it.
QJsonObject Config::defaults() {
    QJsonObject bindings{
        {"key1", QJsonObject{{"press", media("previous")}}},
        {"key2", QJsonObject{{"press", media("play-pause")}}},
        {"key3", QJsonObject{{"press", media("next")}}},
        {"key4", QJsonObject{{"press", keys("ctrl+c")}}},
        {"key5", QJsonObject{{"press", keys("ctrl+v")}}},
        {"key6", QJsonObject{{"press", keys("ctrl+z")}}},
        {"key7", QJsonObject{{"press", keys("ctrl+shift+z")}}},
        {"knob", QJsonObject{{"click", volume("mute", 0)},
                             {"rotate_cw", volume("up")},
                             {"rotate_ccw", volume("down")},
                             {"press_rotate_cw", QJsonObject{{"type", "brightness"}, {"action", "up"}}},
                             {"press_rotate_ccw", QJsonObject{{"type", "brightness"}, {"action", "down"}}}}},
    };
    return QJsonObject{
        {"version", 1},
        {"device", QJsonObject{{"address", QJsonValue::Null}, {"haptics", true}, {"sleep_seconds", 7200},
                               {"long_press_ms", 500}, {"rotate_coalesce_ms", 40}, {"acceleration", "mild"}}},
        {"osd", QJsonObject{{"enabled", true}}},
        {"profiles", QJsonArray{QJsonObject{{"name", "Default"}, {"match", QJsonValue::Null},
                                            {"pages", QJsonArray{QJsonObject{{"name", "Main"}, {"bindings", bindings}}}}}}},
    };
}

bool Config::validSlot(const QString &control, const QString &slot, QString *error) {
    QString base = slot;
    QString modifier;
    const int at = slot.indexOf('@');
    if (at >= 0) {
        base = slot.left(at);
        modifier = slot.mid(at + 1);
    }
    const bool isKey = keyControls().contains(control);
    if (!isKey && control != "knob") {
        if (error) *error = QStringLiteral("unknown control '%1' (key1..key7 or knob)").arg(control);
        return false;
    }
    const QStringList &allowed = isKey ? KeySlots : KnobSlots;
    if (!allowed.contains(base)) {
        if (error) *error = QStringLiteral("%1 has no '%2' slot (use: %3)").arg(control, base, allowed.join(", "));
        return false;
    }
    if (!modifier.isEmpty()) {
        if (control != "knob" || !base.contains("rotate") || !keyControls().contains(modifier)) {
            if (error) *error = QStringLiteral("'@%1' only applies to knob rotation slots, with a key1..key7 modifier").arg(modifier);
            return false;
        }
    }
    return true;
}

bool Config::parseControl(const QString &text, const QString &on, ControlRef *out, QString *error) {
    ControlRef ref;
    const int dot = text.indexOf('.');
    if (dot < 0) {
        ref.control = text;
        ref.slot = on.isEmpty() ? (text == "knob" ? QString("click") : QString("press")) : on;
    } else {
        ref.control = text.left(dot);
        ref.slot = text.mid(dot + 1);
        if (!on.isEmpty() && keyControls().contains(ref.control)) {
            if (error) *error = "use --on for keys, not a dotted slot";
            return false;
        }
    }
    if (!validSlot(ref.control, ref.slot, error))
        return false;
    if (out) *out = ref;
    return true;
}

bool Config::validate(const QJsonObject &doc, QStringList *errors) {
    QStringList problems;
    const int version = doc.contains("version") ? doc.value("version").toInt(-1) : 1;
    if (version != 1)
        problems << QStringLiteral("unsupported config version %1 (this build reads version 1)").arg(version);

    const QJsonObject device = doc.value("device").toObject();
    if (device.contains("long_press_ms") && device.value("long_press_ms").toInt() < 50)
        problems << "device.long_press_ms must be at least 50";
    if (device.contains("rotate_coalesce_ms") && device.value("rotate_coalesce_ms").toInt(-1) < 0)
        problems << "device.rotate_coalesce_ms must be 0 or more";
    if (device.contains("sleep_seconds")) {
        const int s = device.value("sleep_seconds").toInt(-1);
        if (s < 0 || s > 65535) problems << "device.sleep_seconds must be 0..65535";
    }
    if (device.contains("acceleration") && !QStringList{"off", "mild", "strong"}.contains(device.value("acceleration").toString()))
        problems << "device.acceleration must be off, mild or strong";
    if (device.contains("address") && !device.value("address").isNull() &&
        !QRegularExpression("^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$").match(device.value("address").toString()).hasMatch())
        problems << "device.address must be a Bluetooth address like CA:62:68:9B:1B:D3 or null";

    QJsonArray profiles = doc.value("profiles").toArray();
    validateProfiles(profiles, &problems);
    if (errors) *errors = problems;
    return problems.isEmpty();
}

bool Config::validateProfiles(const QJsonArray &profiles, QStringList *problems) {
    QStringList &out = *problems;
    if (profiles.isEmpty()) { out << "profiles must not be empty"; return false; }
    QSet<QString> names;
    bool fallback = false;
    for (int p = 0; p < profiles.size(); ++p) {
        const QString at = QStringLiteral("profiles[%1]").arg(p);
        const QJsonObject profile = profiles[p].toObject();
        const QString name = profile.value("name").toString();
        if (name.isEmpty()) out << at + ".name is missing";
        else if (names.contains(name)) out << QStringLiteral("%1.name '%2' is used twice").arg(at, name);
        names.insert(name);
        const QJsonValue match = profile.value("match");
        if (match.isNull() || match.isUndefined()) {
            fallback = true;
        } else if (!match.isObject()) {
            out << at + ".match must be null or an object";
        } else {
            for (const char *field : {"class", "title"}) {
                const QString pattern = match.toObject().value(field).toString();
                if (!pattern.isEmpty() && !QRegularExpression(pattern).isValid())
                    out << QStringLiteral("%1.match.%2 is not a valid regular expression").arg(at, field);
            }
            if (match.toObject().value("class").toString().isEmpty() && match.toObject().value("title").toString().isEmpty())
                out << at + ".match needs a class or title";
        }
        const QJsonArray pages = profile.value("pages").toArray();
        if (pages.isEmpty()) out << at + ".pages must not be empty";
        for (int g = 0; g < pages.size(); ++g) {
            const QJsonObject bindings = pages[g].toObject().value("bindings").toObject();
            for (auto c = bindings.begin(); c != bindings.end(); ++c) {
                const QJsonObject slotMap = c.value().toObject();
                for (auto s = slotMap.begin(); s != slotMap.end(); ++s) {
                    const QString where = QStringLiteral("%1.pages[%2].bindings.%3.%4").arg(at).arg(g).arg(c.key(), s.key());
                    QString message;
                    if (!validSlot(c.key(), s.key(), &message)) { out << where + ": " + message; continue; }
                    if (!Actions::validate(s.value().toObject(), &message)) out << where + ": " + message;
                }
            }
        }
    }
    if (!fallback) out << "one profile needs \"match\": null to act as the fallback";
    return out.isEmpty();
}

Config::Config(const QString &path, QObject *parent) : QObject(parent), m_path(path) {
    m_doc = defaults();
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(120);
    connect(&m_debounce, &QTimer::timeout, this, &Config::reload);
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_debounce.start(); });
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_debounce.start(); });
}

bool Config::parseBytes(const QByteArray &bytes, QJsonObject *doc, QString *error) const {
    QJsonParseError parseError;
    const QJsonDocument json = QJsonDocument::fromJson(bytes, &parseError);
    if (json.isNull() || !json.isObject()) {
        if (error) *error = QStringLiteral("%1: %2 (offset %3)").arg(m_path, parseError.errorString()).arg(parseError.offset);
        return false;
    }
    QStringList problems;
    if (!validate(json.object(), &problems)) {
        if (error) *error = m_path + ": " + problems.join("; ");
        return false;
    }
    *doc = json.object();
    return true;
}

bool Config::load(QString *error) {
    QFile file(m_path);
    if (!file.exists()) {
        m_doc = defaults();
        return save(error);
    }
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("cannot read %1: %2").arg(m_path, file.errorString());
        return false;
    }
    const QByteArray bytes = file.readAll();
    QJsonObject doc;
    if (!parseBytes(bytes, &doc, error)) return false;
    m_doc = doc;
    m_bytes = bytes;
    arm();
    return true;
}

bool Config::save(QString *error) {
    QStringList problems;
    if (!validate(m_doc, &problems)) {
        if (error) *error = "refusing to write an invalid config: " + problems.join("; ");
        return false;
    }
    if (!QDir().mkpath(QFileInfo(m_path).absolutePath())) {
        if (error) *error = "cannot create " + QFileInfo(m_path).absolutePath();
        return false;
    }
    const QByteArray bytes = QJsonDocument(m_doc).toJson(QJsonDocument::Indented);
    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        if (error) *error = QStringLiteral("cannot write %1: %2").arg(m_path, file.errorString());
        return false;
    }
    m_bytes = bytes;
    arm();
    return true;
}

void Config::arm() {
    if (!m_watching) return;
    // A rename replaces the inode, so the file watch is lost on every save: re-arm both.
    const auto old = m_watcher.files() + m_watcher.directories();
    if (!old.isEmpty()) m_watcher.removePaths(old);
    m_watcher.addPath(QFileInfo(m_path).absolutePath());
    if (QFileInfo::exists(m_path)) m_watcher.addPath(m_path);
}

void Config::setWatching(bool on) {
    m_watching = on;
    if (on) arm();
    else if (!m_watcher.files().isEmpty() || !m_watcher.directories().isEmpty())
        m_watcher.removePaths(m_watcher.files() + m_watcher.directories());
}

void Config::reload() {
    arm();
    QFile file(m_path);
    if (!file.open(QIODevice::ReadOnly)) return;     // mid-rename or deleted; the next event retries
    const QByteArray bytes = file.readAll();
    if (bytes == m_bytes) return;                    // our own save
    QJsonObject doc;
    QString error;
    if (!parseBytes(bytes, &doc, &error)) {
        m_bytes = bytes;                             // report once per distinct broken content
        emit loadFailed(error);
        return;
    }
    m_doc = doc;
    m_bytes = bytes;
    emit changed();
}

int Config::profileIndex(const QString &name) const {
    const QJsonArray list = profiles();
    for (int i = 0; i < list.size(); ++i)
        if (list[i].toObject().value("name").toString() == name) return i;
    return -1;
}

QString Config::defaultProfileName() const {
    for (const auto &p : profiles())
        if (p.toObject().value("match").isNull() || p.toObject().value("match").isUndefined())
            return p.toObject().value("name").toString();
    return profiles().isEmpty() ? QString() : profiles()[0].toObject().value("name").toString();
}

bool Config::replace(const QJsonObject &doc, QString *error) {
    const QJsonObject before = m_doc;
    m_doc = doc;
    if (!save(error)) { m_doc = before; return false; }
    emit changed();
    return true;
}

bool Config::bind(const QString &profile, int page, const ControlRef &ref, const QJsonObject &action, QString *error) {
    QString message;
    if (!validSlot(ref.control, ref.slot, &message) || !Actions::validate(action, &message)) {
        if (error) *error = message;
        return false;
    }
    QJsonArray profileList = profiles();
    const int p = profileIndex(profile);
    if (p < 0) { if (error) *error = QStringLiteral("no profile named '%1'").arg(profile); return false; }
    QJsonObject prof = profileList[p].toObject();
    QJsonArray pages = prof.value("pages").toArray();
    if (page < 0 || page >= pages.size()) {
        if (error) *error = QStringLiteral("profile '%1' has no page %2 (it has %3)").arg(profile).arg(page).arg(pages.size());
        return false;
    }
    QJsonObject pg = pages[page].toObject();
    QJsonObject bindings = pg.value("bindings").toObject();
    QJsonObject control = bindings.value(ref.control).toObject();
    control.insert(ref.slot, action);
    bindings.insert(ref.control, control);
    pg.insert("bindings", bindings);
    pages[page] = pg;
    prof.insert("pages", pages);
    profileList[p] = prof;
    QJsonObject doc = m_doc;
    doc.insert("profiles", profileList);
    return replace(doc, error);
}

bool Config::unbind(const QString &profile, int page, const ControlRef &ref, bool allSlots, QString *error) {
    QJsonArray profileList = profiles();
    const int p = profileIndex(profile);
    if (p < 0) { if (error) *error = QStringLiteral("no profile named '%1'").arg(profile); return false; }
    QJsonObject prof = profileList[p].toObject();
    QJsonArray pages = prof.value("pages").toArray();
    if (page < 0 || page >= pages.size()) {
        if (error) *error = QStringLiteral("profile '%1' has no page %2").arg(profile).arg(page);
        return false;
    }
    QJsonObject pg = pages[page].toObject();
    QJsonObject bindings = pg.value("bindings").toObject();
    QJsonObject control = bindings.value(ref.control).toObject();
    bool removed = false;
    if (allSlots) {
        removed = bindings.contains(ref.control);
        bindings.remove(ref.control);
    } else {
        removed = control.contains(ref.slot);
        control.remove(ref.slot);
        if (control.isEmpty()) bindings.remove(ref.control);
        else bindings.insert(ref.control, control);
    }
    if (!removed) {
        if (error) *error = QStringLiteral("%1 is not bound in profile '%2' page %3").arg(join(ref.control, ref.slot), profile).arg(page);
        return false;
    }
    pg.insert("bindings", bindings);
    pages[page] = pg;
    prof.insert("pages", pages);
    profileList[p] = prof;
    QJsonObject doc = m_doc;
    doc.insert("profiles", profileList);
    return replace(doc, error);
}

bool Config::setDevice(const QString &key, const QJsonValue &value, QString *error) {
    QJsonObject doc = m_doc;
    QJsonObject device = doc.value("device").toObject();
    device.insert(key, value);
    doc.insert("device", device);
    return replace(doc, error);
}
