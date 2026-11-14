// Keyboard-shortcut combos: parsing, keysym validation, and how each delivery
// method (wtype, hyprctl send_shortcut) wants them spelled.
#pragma once

#include <QJsonArray>
#include <QString>
#include <QStringList>
#include <QList>

struct Combo {
    QStringList mods;   // canonical, in the order ctrl, shift, alt, super
    QString key;        // canonical xkb keysym name, e.g. "z", "plus", "XF86AudioMute"

    QString toString() const;                 // "ctrl+shift+z"
    QStringList wtypeArgs() const;            // -M ctrl -M shift -k z -m shift -m ctrl
    QString hyprMods() const;                 // "CTRL SHIFT"
};

// One entry of `hyprctl -j binds` that can be fired by delivering a key combo.
struct HyprBind {
    QString description;
    QString combo;         // canonical, e.g. "super+shift+f"; empty when the bind is not a plain key
    bool usable = false;
};

namespace Shortcut {

// Parses `hyprctl -j binds`. The dispatcher is "__lua" on Hyprland 0.55+, so only the
// key and modmask are used; mouse binds and keycode-only binds are reported unusable.
QList<HyprBind> parseHyprBinds(const QByteArray &json);

// Decodes the bytes a terminal sends for one key press (kitty CSI-u, xterm modifier
// sequences and legacy control bytes). Used by `dialstudio learn`.
bool fromTerminalInput(const QByteArray &bytes, Combo *out, QString *error);

// Rejects typos with a message that names the offending token. Key names are
// checked against xkbcommon's keysym table, so "ctrl+Retrun" fails at bind time.
bool parse(const QString &text, Combo *out, QString *error);

// Canonical keysym name for a user-typed key ("esc" -> "Escape"); empty if unknown.
QString canonicalKey(const QString &name);

// Hyprland modmask bits: shift=1, ctrl=4, alt=8, super=64.
QStringList modsFromMask(int mask);

} // namespace Shortcut
