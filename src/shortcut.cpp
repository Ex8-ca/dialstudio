#include "shortcut.h"

#include <xkbcommon/xkbcommon.h>

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

namespace {

const QStringList ModOrder = {"ctrl", "shift", "alt", "super"};

QString canonicalMod(const QString &raw) {
    const QString m = raw.trimmed().toLower();
    if (m == "ctrl" || m == "control" || m == "ctl") return "ctrl";
    if (m == "shift") return "shift";
    if (m == "alt" || m == "option" || m == "mod1") return "alt";
    if (m == "super" || m == "win" || m == "windows" || m == "logo" || m == "meta" || m == "mod4" || m == "cmd")
        return "super";
    return {};
}

// Friendly spellings people actually type that are not keysym names.
const QHash<QString, QString> &aliases() {
    static const QHash<QString, QString> table = {
        {"esc", "Escape"}, {"enter", "Return"}, {"return", "Return"}, {"del", "Delete"},
        {"pgup", "Prior"}, {"pageup", "Prior"}, {"pgdn", "Next"}, {"pagedown", "Next"},
        {"ins", "Insert"}, {"backspace", "BackSpace"}, {"space", "space"}, {"tab", "Tab"},
        {"up", "Up"}, {"down", "Down"}, {"left", "Left"}, {"right", "Right"},
        {"home", "Home"}, {"end", "End"}, {"print", "Print"},
        {"+", "plus"}, {"-", "minus"}, {"=", "equal"}, {",", "comma"}, {".", "period"},
        {"/", "slash"}, {"\\", "backslash"}, {";", "semicolon"}, {"'", "apostrophe"},
        {"[", "bracketleft"}, {"]", "bracketright"}, {"`", "grave"},
    };
    return table;
}

QString nameOf(xkb_keysym_t sym) {
    char buffer[64];
    return xkb_keysym_get_name(sym, buffer, sizeof buffer) > 0 ? QString::fromLatin1(buffer) : QString();
}

} // namespace

QString Combo::toString() const { return (mods + QStringList{key}).join('+'); }

QStringList Combo::wtypeArgs() const {
    QStringList args;
    auto wtypeMod = [](const QString &m) { return m == "super" ? QStringLiteral("logo") : m; };
    for (const auto &m : mods)
        args << "-M" << wtypeMod(m);
    args << "-k" << key;
    for (int i = mods.size() - 1; i >= 0; --i)
        args << "-m" << wtypeMod(mods[i]);
    return args;
}

QString Combo::hyprMods() const {
    QStringList upper;
    for (const auto &m : mods)
        upper << m.toUpper();
    return upper.join(' ');
}

namespace Shortcut {

QString canonicalKey(const QString &raw) {
    const QString name = raw.trimmed();
    if (name.isEmpty())
        return {};
    if (aliases().contains(name.toLower()))
        return aliases().value(name.toLower());
    // A single letter is the lower-case keysym; shift is a modifier here.
    if (name.size() == 1 && name[0].isLetter())
        return name.toLower();
    const QByteArray latin = name.toLatin1();
    xkb_keysym_t sym = xkb_keysym_from_name(latin.constData(), XKB_KEYSYM_NO_FLAGS);
    if (sym == XKB_KEY_NoSymbol)
        sym = xkb_keysym_from_name(latin.constData(), XKB_KEYSYM_CASE_INSENSITIVE);
    return sym == XKB_KEY_NoSymbol ? QString() : nameOf(sym);
}

bool parse(const QString &text, Combo *out, QString *error) {
    auto fail = [&](const QString &message) {
        if (error) *error = message;
        return false;
    };
    QString s = text.trimmed();
    if (s.isEmpty())
        return fail("empty shortcut");

    // "ctrl++" and a bare "+" mean the plus key.
    QString keyToken;
    QStringList modTokens;
    if (s == "+") {
        keyToken = "+";
    } else if (s.endsWith("++")) {
        keyToken = "+";
        modTokens = s.left(s.size() - 2).split('+', Qt::SkipEmptyParts);
    } else {
        QStringList parts = s.split('+');
        keyToken = parts.takeLast().trimmed();
        modTokens = parts;
    }
    if (keyToken.isEmpty())
        return fail(QStringLiteral("missing key in '%1'").arg(text));

    QSet<QString> mods;
    for (const auto &token : modTokens) {
        const QString m = canonicalMod(token);
        if (m.isEmpty())
            return fail(QStringLiteral("unknown modifier '%1' (use ctrl, shift, alt, super)").arg(token.trimmed()));
        mods.insert(m);
    }
    const QString key = canonicalKey(keyToken);
    if (key.isEmpty())
        return fail(QStringLiteral("unknown key '%1' (not an xkb keysym name)").arg(keyToken));

    Combo combo;
    for (const auto &m : ModOrder)
        if (mods.contains(m))
            combo.mods << m;
    combo.key = key;
    if (out) *out = combo;
    return true;
}

QStringList modsFromMask(int mask) {
    QStringList mods;
    if (mask & 4) mods << "ctrl";
    if (mask & 1) mods << "shift";
    if (mask & 8) mods << "alt";
    if (mask & 64) mods << "super";
    return mods;
}

} // namespace Shortcut

namespace Shortcut {

QList<HyprBind> parseHyprBinds(const QByteArray &json) {
    QList<HyprBind> out;
    for (const auto &item : QJsonDocument::fromJson(json).array()) {
        const QJsonObject bind = item.toObject();
        HyprBind result;
        result.description = bind.value("description").toString();
        const QString key = bind.value("key").toString();
        const bool mouse = bind.value("mouse").toBool() || key.startsWith("mouse");
        const QString name = (mouse || key.isEmpty() || key.startsWith("code:")) ? QString() : canonicalKey(key);
        if (!name.isEmpty()) {
            Combo combo;
            combo.mods = modsFromMask(bind.value("modmask").toInt());
            // keep the canonical modifier order
            QStringList ordered;
            for (const auto &m : ModOrder) if (combo.mods.contains(m)) ordered << m;
            combo.mods = ordered;
            combo.key = name;
            result.combo = combo.toString();
            result.usable = true;
        }
        out << result;
    }
    return out;
}

bool fromTerminalInput(const QByteArray &bytes, Combo *out, QString *error) {
    auto fail = [&](const QString &m) { if (error) *error = m; return false; };
    if (bytes.isEmpty()) return fail("no input");
    QSet<QString> mods;
    QString key;
    auto applyMask = [&](int mask) {
        if (mask & 1) mods.insert("shift");
        if (mask & 2) mods.insert("alt");
        if (mask & 4) mods.insert("ctrl");
        if (mask & 8) mods.insert("super");
    };
    auto fromCodepoint = [&](int cp) -> QString {
        switch (cp) {
        case 27: return "Escape";
        case 13: return "Return";
        case 9: return "Tab";
        case 127: return "BackSpace";
        case 32: return "space";
        default: return canonicalKey(QString(QChar(cp)));
        }
    };

    if (bytes.size() >= 3 && bytes[0] == 0x1b && bytes[1] == '[') {
        const QByteArray body = bytes.mid(2, bytes.size() - 3);
        const char final = bytes.back();
        const QList<QByteArray> parts = body.split(';');
        const int first = parts.value(0).split(':').value(0).toInt();
        const int modValue = parts.size() > 1 ? parts[1].split(':').value(0).toInt() : 1;
        applyMask(qMax(modValue, 1) - 1);
        if (final == 'u') {
            key = fromCodepoint(first);
        } else if (final == '~') {
            static const QHash<int, QString> tilde = {{2, "Insert"}, {3, "Delete"}, {5, "Prior"}, {6, "Next"},
                {15, "F5"}, {17, "F6"}, {18, "F7"}, {19, "F8"}, {20, "F9"}, {21, "F10"}, {23, "F11"}, {24, "F12"}};
            key = tilde.value(first);
        } else {
            static const QHash<char, QString> letters = {{'A', "Up"}, {'B', "Down"}, {'C', "Right"}, {'D', "Left"},
                {'H', "Home"}, {'F', "End"}, {'P', "F1"}, {'Q', "F2"}, {'S', "F4"}};
            key = letters.value(final);
        }
    } else if (bytes.size() == 3 && bytes[0] == 0x1b && bytes[1] == 'O') {
        static const QHash<char, QString> ss3 = {{'P', "F1"}, {'Q', "F2"}, {'R', "F3"}, {'S', "F4"},
            {'A', "Up"}, {'B', "Down"}, {'C', "Right"}, {'D', "Left"}};
        key = ss3.value(bytes[2]);
    } else if (bytes.size() == 2 && bytes[0] == 0x1b) {
        mods.insert("alt");
        const quint8 c = quint8(bytes[1]);
        if (c >= 'A' && c <= 'Z') { mods.insert("shift"); key = QString(QChar(c)).toLower(); }
        else key = fromCodepoint(c);
    } else if (bytes.size() == 1) {
        const quint8 c = quint8(bytes[0]);
        if (c >= 1 && c <= 26 && c != 9 && c != 13) { mods.insert("ctrl"); key = QString(QChar('a' + c - 1)); }
        else if (c >= 'A' && c <= 'Z') { mods.insert("shift"); key = QString(QChar(c)).toLower(); }
        else key = fromCodepoint(c);
    }
    if (key.isEmpty()) return fail("could not decode that key press (" + QString::fromLatin1(bytes.toHex(' ')) + ")");
    Combo combo;
    for (const auto &m : ModOrder) if (mods.contains(m)) combo.mods << m;
    combo.key = key;
    if (out) *out = combo;
    return true;
}

} // namespace Shortcut
