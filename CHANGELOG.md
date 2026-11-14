# Changelog

## 0.2.0

- Profiles pane in the window: profile list with the active one marked, add / rename / delete,
  window-class match rule with a "pick a running window" source (hyprctl clients), pages per
  profile (add / rename / remove), pin/auto of the active profile. One validated daemon save per
  edit; a refusal keeps the saved list.
- Presets: named snapshots of the whole profile list (`~/.config/dialstudio/presets/*.json`) with
  save / apply / delete in the pane, over the IPC (`presets`) op.
- The launch action gained an app picker: visible .desktop entries (XDG dirs + Flatpak exports,
  localized names) store the .desktop id and launch via uwsm-app / gtk-launch.
- Sleep timer dropdown in the header (never / 1-4 h), reading and writing the dial's own setting.
- Key captions read "Ctrl+Shift" over a big "Z" instead of three cramped lines; the inspector no
  longer clips its last row, and pressure rows align with the heading.
- New IPC ops: `profiles_get`, `profiles_set`, `presets` (list/save/load/apply/delete/rename),
  `apps`, `clients`; the model re-asks if an early reply races a reconnect.
- Tests: 225 (`bin/test`), up from 205.

## 0.1.5

- First window, `dialstudio open`: dial drawing (original SVG) with clickable keys and knob, header
  with connection pill, battery, firmware and haptics switch, inspector with an action picker
  (shortcut, Omarchy shortcut, volume, media, brightness, launch, Hyprland, Omarchy command, shell,
  nothing), shortcut recorder with modifier chips, Test button, undo/redo, `?` overlay, banner with
  a Start service button when the daemon is not running. Follows the Omarchy accent, Hyprland
  rounding and language (Danish catalog).
- New IPC ops: `bindings_get`, `bindings_set`, `test`, structured `input` events (control, kind,
  direction, steps), `shortcuts` with a filter, and `inject` (only with `DIALSTUDIO_DEV=1`, for tests).
- `bind --omarchy` now runs the shortcut's own command or Hyprland dispatch instead of delivering its
  key combo, which Hyprland ignores. The catalog is read from the Lua config; unrunnable binds are
  listed as such and refused at bind time. Links are re-resolved by description on every Hyprland
  config reload, and a missing or no longer runnable one shows in `status`.
- Config: `{"type":"omarchy_bind","description":...}`. The v0.1.0 spelling
  `{"type":"keys","omarchy":...}` still loads and runs the command; `bind` upgrades it.
- Package depends on `qt6-declarative`, `qt6-svg`, `qt6-wayland` and `lua`.

## 0.1.0

- Headless daemon (`dialstudio daemon`): BlueZ link by UUID (no handles), 1 s heartbeat, reconnect
  with 2/4/8..30 s backoff, clean StopNotify on SIGTERM so the dial returns to factory mode.
- Input state: held-key set, long press, press-rotate fallthrough, modifier-rotate, rotation
  coalescing with optional acceleration.
- JSON config with validation, atomic save and hot reload; defaults reproduce factory behaviour.
- Actions: volume, mic, brightness, media, keys, text, hypr (Lua), hypr_zoom, omarchy, exec, url,
  shell, page, profile, haptics, multi, noop.
- Profiles with Hyprland class/title matching via socket2.
- CLI: daemon, status, events, set, profile, page, doctor, bind, unbind, bindings, send,
  shortcuts, learn.
- IPC over a Unix socket, newline-delimited JSON.
- PKGBUILD, desktop entry, systemd user unit, install hook, install.sh.

## 0.0.1

- TEA codec and 20-byte packet framing (`teacodec`).
- Protocol parser and command builders (`protocol`): key, knob and rotation
  events, heartbeat, haptics, sleep, firmware, serial fragments, battery.
- Qt Test suite against every vector in PLAN.md section 2 and
  `research/PROTOCOL-kriomant.md` section 3.
