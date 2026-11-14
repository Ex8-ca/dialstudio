# Dial Studio: Ulanzi D100H controller app for Omarchy

Implementation plan, written for an agent to execute. Read all of it before
writing code. Everything under "Verified facts" was checked on Marc's machine
on 2026-11-13. Do not re-derive it, and do not "fix" it.

---

## 0. Goal

Build a Linux replacement for Ulanzi Studio (https://www.ulanzi.com/pages/ulanzi-app)
for the **Ulanzi D100H Dial** (7 keys, one push-rotate knob, Bluetooth LE only).
It should:

1. Take over the dial (put it in "online mode"), so each of the 7 keys plus the
   knob's turn, push-turn and click can do anything: run commands, Hyprland
   dispatches, keystrokes, volume, media, brightness, switching pages.
2. Have a GUI editor like Ulanzi Studio: a picture of the dial; click a control
   and assign an action from a categorised action library. Profiles switch
   automatically with the focused app, and a live preview lights up as you
   press real keys.
3. Install with one command on Omarchy and show up in the app launcher. A
   systemd user service starts the daemon.
4. Fall back cleanly. If the daemon stops, the dial goes back to its factory
   volume/media behaviour within about 3 s (firmware does this, not us).

Name: **`dialstudio`**. It is the binary, package, `.desktop` file, icon and
Wayland app id.

Repo: `~/Work/dialstudio` (already `git init`'d, with `research/` holding the
verified spike and the upstream protocol doc).

---

## 1. Verified facts (Marc's machine, 2026-11-13)

| Fact | Value |
|---|---|
| OS | Omarchy, kernel 7.2.15-1-omarchy, Hyprland 0.56.2 |
| Dial | Paired, bonded, trusted, connected. `CA:62:68:9B:1B:D3`, name `Ulanzi Dial` |
| HID identity | `0005:FFF1:0082` (BT bus), currently `/dev/hidraw4`, root-only (we don't need it) |
| BlueZ vendor service | `00001971-…` at `/org/bluez/hci0/dev_CA_62_68_9B_1B_D3/service004b` |
| Notify char | `0000fd00-…` (`char004c`), flags `read, notify` |
| Write char | `0000fd01-…` (`char004f`), flags `read, write-without-response, write` |
| Access | **Works as user `marc` over the system D-Bus with no root, no udev rule, no group change** |
| Firmware | 1.3.8.1 (custom code `400037`) |
| Haptics / sleep | on (`ff`) / 7200 s |
| Installed tools | `wtype`, `wpctl`, `hyprctl`, `brightnessctl`, `notify-send`, `omarchy-pkg-add`, `qmake6`, `qt6-base 6.12`, `qt6-declarative 6.12` |
| Missing tools | `playerctl`, `swayosd-client`, `ydotool` (make them optional, not required) |

Spike output (`research/spike_bluez.py`, run with `uv run --with dbus-fast`):
```
RX 0601231001000000000000000000000000000000   heartbeat reply
RX 0104041140003728280101030826040818000100   firmware version -> 1.3.8.1
RX 01064011ff000000000000000000000000000000   haptics = on
RX 01014211201c0000000000000000000000000000   sleep = 0x1c20 = 7200 s
```
So the TEA codec, UUIDs, heartbeat and settings reads are all confirmed on
Linux/BlueZ. **Do not look up characteristic paths by handle (`char004c`).
Discover them by UUID at runtime.** The handles are only valid for this pairing.

---

## 2. Protocol summary (from kriomant/ulanzi-d100h PROTOCOL.md, verified)

Full detail is in `research/PROTOCOL-kriomant.md`. What the implementation needs:

**Wire format.** Every vendor packet is 20 bytes. Encrypt bytes 0–7 and 8–15
**independently** with original TEA (32 rounds, delta `0x9e3779b9`, LE words).
Bytes 16–19 are passed through unchanged.
Key bytes: `ca ba a5 ca 6d 8a 2a bc ba 9e 5a ca ca 8b b8 9b`.
Test vector: plaintext `8b10010100…00` encrypts to
`8e8f5e16ec67281e3890c499a360aaad00000000`.

**Online mode.** After `StartNotify` on `fd00`, write a heartbeat
`06 01 23 00 01` (zero-padded, encrypted) to `fd01` with `type=command`
(write-without-response) **immediately and then every 1 s**. While the
heartbeats continue, the factory HID actions are suppressed and vendor events
arrive. About 3 s after they stop, the dial reverts to factory HID.
Replies `06 01 23 10 xx` are heartbeat acks; ignore them.

**Control events.** Decrypted form is `8b 10 ID STATE`, where STATE is 01 for
down/step and 00 for up.

| ID | Control | Notes |
|---|---|---|
| 01 / 02 / 03 | Top-left / top-middle / top-right key | real down/up |
| 04 / 05 | Upper-left / upper-right side key | real down/up |
| 06 / 08 | Lower-left / lower-right side key | real down/up |
| 07 | Knob click | **synthetic down+up pair sent on release**. It is suppressed if the knob was turned while held, so treat `07 01` as "click" and ignore `07 00` |
| 09 / 0a | Rotate CCW / CW | one event per detent, no up event |
| 0b / 0c | Rotate CCW / CW **while knob pressed** | separate IDs |
| 41–44 | Offline/standalone IDs | only seen when NOT in online mode. Ignore them |

Keys can be held together (all 7 at once has been verified). Track a set of
held keys for chords and modifier+rotate. Clear the set on disconnect.

**Settings and queries** (plaintext prefix, zero-pad, encrypt):

| Op | Send | Reply |
|---|---|---|
| Read haptics | `01 06 40 01` | `01 06 40 11 VV` (00 off / ff on) |
| Set haptics | `01 06 40 04 VV` | `01 06 40 14 VV` |
| Read sleep | `01 01 42 01` | `01 01 42 11 LO HI` (u16 seconds) |
| Set sleep | `01 01 42 02 LO HI` | `01 01 42 12 LO HI` |
| Firmware | `01 04 04 01` | `01 04 04 11` with fw at offsets 10, 11, 12 and optional 4th part at offset 18 |
| Serial | `01 01 0b 01` | fragmented. Dedupe fragments by index |
| Battery | (unsolicited) `0b 7b VLO VHI FLAGS PCT` | flags: bits0-1 low, bit2 power-off, bit3 charging, bit4 full |

Battery is also exposed by BlueZ `org.bluez.Battery1.Percentage`. Use that as
the primary source and the `0b 7b` reports for the charging flag.

**Unknown, and not needed for v0.1:** the 7 keys' RGB LEDs. Nobody has captured
an LED command yet. See milestone M5.

---

## 3. Architecture

One C++17 / Qt 6 binary (Omarchy app convention: qmake6 + make, no CMake),
with subcommands in the style of Hype:

```
dialstudio daemon         headless: BlueZ link + heartbeat + action engine + IPC (systemd runs this)
dialstudio open           GUI editor (default when launched from the .desktop)
dialstudio status --json  device, battery, fw, haptics, sleep, active profile
dialstudio events         stream decoded events to stdout (debug / scripting)
dialstudio set haptics on|off ; dialstudio set sleep <seconds>
dialstudio profile <name> ; dialstudio page next|prev|<n>
dialstudio doctor         check bluez, pairing, service char, tools, systemd unit
dialstudio skill install  install a short agent skill (Omarchy convention)
```

Why this design:
- **Qt for everything.** `QtDBus` (part of `qt6-base`, already installed) talks
  to BlueZ, so no Python, no Rust and no extra runtime deps. That keeps it to
  one package and one language.
- **The daemon and GUI are separate processes in the same binary.** The heartbeat
  must never stop just because the editor window closed. The GUI is a client of
  the daemon.
- IPC uses `QLocalServer` at `$XDG_RUNTIME_DIR/dialstudio.sock`, with
  newline-delimited JSON. Messages: `{"op":"status"}`, `{"op":"subscribe"}` →
  event stream, `{"op":"reload"}`, `{"op":"set",...}`, `{"op":"profile",...}`.
  The CLI subcommands use the same socket.

### Source layout (flat `src/`, per the omarchy-app skill)

```
dialstudio.pro
bin/build  bin/test  bin/install  bin/dev-run
src/main.cpp            subcommand dispatch; QT_QPA_PLATFORM=offscreen for non-GUI cmds
src/teacodec.{h,cpp}    TEA + packet build/parse (pure, unit-tested against vectors)
src/protocol.{h,cpp}    Message/Event/Reply types, Command builders, parser
src/bluezlink.{h,cpp}   ObjectManager watch, find device by service UUID 1971, resolve
                        fd00/fd01 by UUID, StartNotify, PropertiesChanged→bytes,
                        WriteValue(type=command), heartbeat QTimer(1000ms),
                        reconnect state machine
src/inputstate.{h,cpp}  held-key set, click/press-rotate semantics, rotation coalescing
                        & acceleration, long-press detection (client-side timer), chords
src/config.{h,cpp}      JSON config load/validate/save-atomic, QFileSystemWatcher hot reload
src/profiles.{h,cpp}    profile + page resolution, Hyprland socket2 activewindow listener
src/actions.{h,cpp}     action registry + executors (see §5)
src/ipcserver.{h,cpp}   QLocalServer JSON-lines
src/ipcclient.{h,cpp}   used by GUI + CLI
src/daemon.{h,cpp}      wires link → inputstate → profiles → actions; publishes events
src/backend.{h,cpp}     GUI QObject: config model, live state, Q_INVOKABLE edits
src/theme.{h,cpp}       Omarchy accent + hyprctl rounding (from omarchy-app template)
src/translator.{h,cpp}  Omarchy language following (from omarchy-app template)
src/Main.qml            window: left profiles, centre dial, right action library/inspector
src/DialView.qml        SVG dial with 7 keys + knob, hit areas, live highlight
src/ActionLibrary.qml   categorised, searchable action list (drag or click to assign)
src/ActionInspector.qml per-action parameter form
src/assets/dial.svg     hand-drawn top-down D100H (no Ulanzi artwork)
src/resources.qrc  src/i18n/*.json
tests/dialstudio_tests.{pro,cpp}
pkgbuild/PKGBUILD  pkgbuild/dialstudio.desktop  pkgbuild/dialstudio.svg
pkgbuild/dialstudio.service  pkgbuild/dialstudio.install
install.sh  README.md  LICENSE(MIT)  CHANGELOG.md  ROADMAP.md
```

### BlueZ link details (where the real bugs will be)

- Use `org.freedesktop.DBus.ObjectManager.GetManagedObjects` on `org.bluez /`,
  then subscribe to `InterfacesAdded` / `InterfacesRemoved` and to
  `PropertiesChanged` on device objects.
- Find the device: an `org.bluez.Device1` that has a child `GattService1` with
  UUID `00001971-0000-1000-8000-00805f9b34fb`. Fall back to `Name` starting
  with "Ulanzi Dial" or Modalias `usb:vFFF1p0082`.
- Wait for `ServicesResolved == true` before resolving the characteristics.
- `StartNotify()` on `fd00`. Values arrive as
  `PropertiesChanged(org.bluez.GattCharacteristic1, {Value: ay})`. Decrypt and
  parse. **Every 20-byte notify is a separate message. Don't buffer them.**
- Write with `WriteValue(ay, {"type": <s>"command"})`. If it fails with
  `org.bluez.Error.NotPermitted`, fall back to `"request"`.
- Heartbeat: a 1000 ms `QTimer`, started after `StartNotify` succeeds and
  stopped on `Connected=false`. Send one heartbeat immediately at start.
- When the device is not connected, call `Device1.Connect()` with backoff
  (2, 4, 8 … capped at 30 s). Don't fight the user if they turn Bluetooth off;
  watch `Adapter1.Powered`.
- On `SIGTERM`, stop the heartbeat and call `StopNotify`. The dial then falls
  back to factory mode on its own.
- The dial pairs with up to 3 hosts, selected by a switch underneath. If it
  disconnects because the user switched host, that's normal. Just wait.
- **Never call `Device1.Pair`/`Remove` automatically.** Pairing is a user action
  that goes through the GUI's "Pair a dial" sheet (scan with
  `Adapter1.StartDiscovery`, filter by UUID 1971 or name, then `Pair`, `Trust`,
  `Connect`), or through `omarchy`'s own Bluetooth TUI.

### Input semantics (inputstate)

- Key down/up gives `press`, `release`, and `long_press` (client timer, default
  500 ms, configurable; when a long_press binding exists, `press` fires on
  release if the hold was short. This is the same model as Ulanzi Studio).
- Rotation gives `rotate_cw`/`rotate_ccw` with a `steps` count, coalesced over
  `rotate_coalesce_ms` (default 40 ms). An optional acceleration curve
  multiplies the steps when they come faster than 30 ms apart (fast rotation
  produced events about 14 ms apart).
- Press-rotate (`0b`/`0c`) is a separate binding. If it's unset, fall through
  to plain rotate (as ulanzi-studio-niri does).
- Modifier-rotate: if a key is held while rotating, look up
  `rotate_cw@<key>` first. If that binding exists, the key's own `press` is
  cancelled on release (it acted as a modifier).
- Knob `click`: fire on `07 01`, ignore `07 00`.

---

## 4. Config model (`~/.config/dialstudio/config.json`)

Use JSON (native to Qt; the GUI is the main editor; keep it human-readable
with 2-space indents). Write it atomically. Hot-reload it in the daemon.

```jsonc
{
  "version": 1,
  "device": { "address": null, "haptics": true, "sleep_seconds": 7200,
              "long_press_ms": 500, "rotate_coalesce_ms": 40, "acceleration": "mild" },
  "osd": { "enabled": true },                 // notify-send / layer-shell toast on page/profile change
  "profiles": [
    { "name": "Default", "match": null,      // fallback profile
      "pages": [ { "name": "Main", "bindings": {
          "key1":  { "press": { "type": "media", "action": "previous" } },
          "key2":  { "press": { "type": "media", "action": "play-pause" } },
          "key3":  { "press": { "type": "media", "action": "next" } },
          "key4":  { "press": { "type": "keys", "combo": "ctrl+c" } },
          "key5":  { "press": { "type": "keys", "combo": "ctrl+v" } },
          "key6":  { "press": { "type": "keys", "combo": "ctrl+z" } },
          "key7":  { "press": { "type": "keys", "combo": "ctrl+shift+z" } },
          "knob":  { "click": { "type": "volume", "action": "mute" },
                     "rotate_cw":  { "type": "volume", "action": "up", "step": 2 },
                     "rotate_ccw": { "type": "volume", "action": "down", "step": 2 },
                     "press_rotate_cw":  { "type": "brightness", "action": "up" },
                     "press_rotate_ccw": { "type": "brightness", "action": "down" } }
      } } ] },
    { "name": "Kdenlive", "match": { "class": "^org\\.kde\\.kdenlive$" }, "pages": [ ... ] }
  ]
}
```

Control naming follows a stable physical layout. Put it in `README`:
`key1..key3` = top row left→right (IDs 01, 02, 03); `key4` = upper-left (04),
`key5` = upper-right (05), `key6` = lower-left (06), `key7` = lower-right (08).
The first-run default config **reproduces the factory behaviour**, so nothing
changes until the user edits it.

Profile selection: listen to Hyprland's socket2
(`$XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/.socket2.sock`,
`activewindow>>class,title`). Match profiles by regex on class (and optionally
title). Pick the first match, else Default. A manual `dialstudio profile X`
pins a profile until `dialstudio profile auto`.

---

## 5. Action library (what the GUI's right panel lists)

Each action is `{type, ...params}`, with an executor in `actions.cpp`. Run
every action through `QProcess::startDetached` (never through a shell unless
the type is `shell`), with a 5 s watchdog. Log failures to the journal and
show them in the GUI's status bar.

| Category | type | Implementation |
|---|---|---|
| System | `volume` up/down/mute (+step) | `wpctl set-volume -l 1.5 @DEFAULT_AUDIO_SINK@ 2%+`, `wpctl set-mute … toggle`. Use `swayosd-client` if present for the OSD |
| System | `mic` mute toggle | `wpctl set-mute @DEFAULT_AUDIO_SOURCE@ toggle` |
| System | `brightness` up/down | `brightnessctl set 5%+` (or `swayosd-client --brightness`) |
| Media | `media` play-pause/next/previous/stop | `playerctl` if installed, else `wtype -k XF86AudioPlay` etc. |
| Keyboard | `keys` combo / `text` string | `wtype -M ctrl -k c -m ctrl`; parse `ctrl+shift+z` |
| Hyprland | `hypr` dispatch + arg | `hyprctl dispatch workspace +1`, `movefocus l`, `togglefloating`, `fullscreen`, `killactive`, `exec` |
| Hyprland | `hypr_zoom` in/out (rotation) | `hyprctl keyword cursor:zoom_factor` (clamped 1.0–5.0) |
| Omarchy | `omarchy` menu / screenshot / screenrecord / theme-next / lock | the matching `omarchy-*` command (discover them with `compgen -c omarchy-` and list them in the GUI) |
| Launch | `exec` app | `uwsm-app -- <cmd>` if present (Omarchy launches apps that way), else direct |
| Launch | `url` | `xdg-open` |
| Script | `shell` command | `sh -c`; the only shell-interpreted type; marked "advanced" in the GUI |
| Dial | `page` next/prev/goto, `profile` goto/auto | internal |
| Dial | `haptics` toggle | internal; sends the set-haptics command |
| Dial | `multi` [actions…] with delay_ms | internal sequencer |
| Dial | `noop` | swallow (useful on press_rotate to block fallthrough) |

### 5a. Shortcut binding interface (v0.1, CLI + IPC; GUI recorder in v0.2)

A first-class way to link a dial control to a keyboard shortcut, with no JSON
editing. The CLI, the GUI and any script all call the same IPC op.

```
dialstudio bind <control> <combo> [--on press|long_press|release]
                                  [--profile NAME] [--page N] [--via wtype|hypr]
dialstudio bind knob.rotate_cw  ctrl+plus          # controls: key1..key7, knob.click,
dialstudio bind knob.press_rotate_ccw super+shift+left   # knob.rotate_cw/ccw, knob.press_rotate_cw/ccw
dialstudio bind key4 --omarchy "Terminal"          # link to an EXISTING Hyprland keybind
dialstudio unbind <control> [--profile ...]
dialstudio bindings [--json] [--profile ...]       # list current links
dialstudio send <combo> [--via ...]                # fire a shortcut now (test)
dialstudio shortcuts [--json]                      # list Omarchy/Hyprland binds (from `hyprctl binds -j`)
dialstudio learn <control>                         # prompt, then bind whatever combo the user presses
```

IPC: `{"op":"bind","control":"key4","on":"press","action":{"type":"keys","combo":"ctrl+shift+z"},"profile":"Default","page":0}`
(also `unbind`, `bindings`, `send`, `shortcuts`). Every successful bind saves
the config atomically and hot-reloads, so it works while the daemon runs.

Three ways to deliver a shortcut. Each is a `keys` action option (`via`):

1. **`wtype`** (default): synthesises the key press to the focused window.
   Simplest; works in any app. Combo parser: `ctrl+shift+z`, `super+1`,
   `alt+Tab`, `XF86AudioMute`. Validate the key names against xkb keysyms at
   bind time and reject typos with a clear error.
2. **`hypr`**: `hyprctl dispatch sendshortcut <mods>,<key>,<window>`. Targets
   a specific window (`class:^firefox$`, or the active one) without needing
   focus, and does not depend on wtype.
3. **`omarchy` link**. SUPERSEDED in v0.1.5: injected keys never fire Hyprland binds (tested), so the
   link now reads the Lua source and RUNS the bind's command or dispatch. See README, "Shortcut
   delivery", and CHANGELOG 0.1.5. The text below is the original plan and is kept for history. Read
   `hyprctl binds -j` and let the user pick an existing bind by its
   `description` (254 of 277 have one, e.g. "Volume up"). **Verified on this
   machine: every bind reports `dispatcher: "__lua"` with a numeric `arg`
   (Hyprland 0.56 Lua config), so the dispatcher cannot be recovered and run
   directly. Do not try.** Instead store the bind's `key` + `modmask` (decode
   the modmask to names: shift=1, ctrl=4, alt=8, super=64) plus the description,
   and fire it by delivering that combo with `hyprctl dispatch sendshortcut`
   (or wtype). On each config reload, re-resolve by description; if the combo
   changed, use the new one, and if the description is gone, warn in
   `status`/the GUI.

GUI (M2): the inspector's "Keyboard shortcut" action has a **Record** button.
Click it, press the combo on the real keyboard, and it fills the field
(Qt `keyPressEvent`, with modifiers shown as chips). Next to it is a **Pick an
Omarchy shortcut…** searchable list from `shortcuts`. **Test** calls `send`.

Open point, NOT yet tested: whether Hyprland fires its own keybinds from
`wtype`-injected keys or from `sendshortcut`. Test both in M1 with a harmless
bind (e.g. one that runs `notify-send`) before relying on either. If neither
fires binds, the `omarchy` link mode needs a different route, such as
resolving the description to the `omarchy-*` command it runs (look in the
Omarchy bindings config under `~/.local/share/omarchy/` and `~/.config/hypr/`),
and the plan should be updated with whatever works. Record the result in
`docs/hardware-test.md`.

Plugin extensibility (v0.3): any executable in
`~/.config/dialstudio/actions/*` that answers `--describe` with JSON
(`{name, category, params:[…]}`) shows up in the library. It gets invoked with
the params as JSON on stdin. This gives us an "Ulanzi plugins" equivalent
without having to implement their Electron/WebSocket SDK.

---

## 6. GUI (the "Studio" part)

Follow the `omarchy-app` skill (load it):
`QGuiApplication`, `setDesktopFileName("dialstudio")`, Material Dark with
accent from `~/.local/state/omarchy/current/theme/colors.toml` (watched live),
radius from `hyprctl getoption decoration:rounding`, all strings `qsTr`,
`?` overlay for shortcuts, `Q` to quit (ask first if there are unsaved edits),
Ctrl+Z / Ctrl+Shift+Z undo/redo of config edits.

Layout, modelled on Ulanzi Studio's three-pane editor:
- **Left:** profiles list (+ add / duplicate / delete; the match-rule editor
  has a "pick from running windows" dropdown populated from `hyprctl clients -j`),
  with pages as tabs under the selected profile.
- **Centre:** `DialView`, a top-down SVG of the D100H. Each key and the knob is
  a hit target showing its bound action's icon and label. Selecting the knob
  shows sub-slots for click / CW / CCW / press-CW / press-CCW, and for keys,
  press / long-press. **Live mode:** while the daemon is connected, real presses
  flash the matching key and turning rotates the knob graphic.
  This is the brendanwelsh "dial viewer" idea, drawn natively.
- **Right:** the action library (categories from §5, a search box), then the
  inspector form for the selected slot. Use "Test" to fire the action now
  through the daemon.
- **Header:** connection pill (connected / searching / Bluetooth off / not
  paired → "Pair a dial…"), battery % + charging icon, firmware, haptics toggle,
  sleep timer dropdown (Off / 1 h / 2 h / 3 h / 4 h, matching Studio).
- Edits apply to an in-memory model. Save (Ctrl+S, plus autosave after 1 s idle)
  writes atomically, and the daemon hot-reloads. Saving never needs a restart.
- If the daemon isn't running, show a banner with a "Start service" button
  (`systemctl --user enable --now dialstudio`).

Icon: draw an original SVG (a dial with a 3+4 key outline). Do not ship
Ulanzi logos or product photos.

---

## 7. Install story ("installs easily")

**Primary:** an Arch package built locally.
```
git clone https://github.com/Ex8-ca/dialstudio && cd dialstudio && bin/install
```
`bin/install` runs `makepkg -fsi`. The PKGBUILD has
`depends=(qt6-base qt6-declarative qt6-svg qt6-wayland bluez bluez-utils wtype)`
and `optdepends=(playerctl swayosd brightnessctl)`. `makedepends=(base-devel)`.
It installs:
- `/usr/bin/dialstudio`
- `/usr/share/applications/dialstudio.desktop` (Exec=`dialstudio open`)
- `/usr/share/icons/hicolor/scalable/apps/dialstudio.svg`
- `/usr/lib/systemd/user/dialstudio.service` (`ExecStart=/usr/bin/dialstudio daemon`,
  `Restart=on-failure`, `After=graphical-session.target bluetooth.target`,
  `WantedBy=graphical-session.target`, `PartOf=graphical-session.target`;
  **no** ProtectHome/ProtectSystem, because actions spawn user apps)
- a `.install` file whose post_install message prints:
  `systemctl --user enable --now dialstudio`

**One-liner:** `install.sh`
(`curl -fsSL https://raw.githubusercontent.com/Ex8-ca/dialstudio/main/install.sh | bash`).
It checks `pacman`, installs the build deps with `omarchy-pkg-add` (or `sudo pacman -S --needed`),
clones to `~/.local/share/dialstudio/src`, runs `makepkg -fsi`, then
`systemctl --user enable --now dialstudio`, then `dialstudio doctor`.
It must be idempotent: re-running it upgrades.

**First run:** the daemon writes the default config if it's missing. If no
dial is paired, the GUI opens straight to the "Pair a dial" sheet.

**Later (M4):** publish to the AUR as `dialstudio` (and `dialstudio-git`),
then propose it to `omacom/omarchy-pkgs`, per the omarchy-app skill's release
section.

No udev rule is needed (verified: BlueZ D-Bus access works for the logged-in
user). Don't add one.

---

## 8. Milestones (tag each; CHANGELOG entry per tag; separate commits per concern)

**M0 / v0.0.1: protocol core.**
`teacodec`, `protocol`, Qt Test unit tests against the vectors in §2 and
`research/PROTOCOL-kriomant.md` §3 (all 4 vectors, plus the 4 replies captured
in §1). `bin/build`, `bin/test`. Done when `bin/test` is green.

**M1 / v0.1.0: headless daemon (usable without a GUI).**
`bluezlink`, `inputstate`, `config` (JSON + hot reload), `actions` (§5 minus
plugins), `profiles` (static default plus Hyprland class matching), `ipcserver`,
and the CLI (`daemon`, `status`, `events`, `set`, `profile`, `page`, `doctor`,
plus the §5a shortcut interface: `bind`, `unbind`, `bindings`, `send`,
`shortcuts`, `learn`),
plus the systemd unit, PKGBUILD and `install.sh`.
Acceptance, on the real dial:
1. `dialstudio events` prints `key1 press`/`release` for each key, `knob click`,
   `rotate cw steps=N`, and `press_rotate`.
2. Turning the knob with the default config changes the volume (via wpctl,
   not factory HID). After `systemctl --user stop dialstudio`, the factory
   volume works again within about 3 s.
3. Toggling the dial's Bluetooth off and on reconnects within 30 s without a
   restart.
4. `dialstudio set haptics off` gets read back as off from `status`.
5. Focusing a window matched by a profile switches bindings; `status` shows it.

**M1.5 / v0.1.5: first window (visual interface early). DONE, see CHANGELOG and docs/gui-test.md.**
Build the GUI skeleton right after the daemon works, so you can see and click
things before the full editor exists: `Main.qml` with `DialView` (the SVG dial,
live key flash and knob rotation from the daemon's event stream), the header
pill (connection, battery, firmware), and a minimal inspector with the
shortcut Record button and the Omarchy-shortcut picker (§5a). Acceptance: open
`dialstudio open`, press a real key and see it light up; click key4, record
`ctrl+shift+z`, press the key and the shortcut fires. M2 then adds profiles,
pages, the full action library, presets and polish.

**M2 / v0.2.0: GUI editor.**
Everything in §6. Done when Marc can bind key4 to `hyprctl dispatch workspace +1`
from the GUI with no JSON hand-editing, see it flash live, and survive a theme
switch and a language switch (check both with screenshots, per the
omarchy-app loop).

**M3 / v0.3.0: Studio parity extras.**
Multi-page per profile with a page OSD, `multi` macro sequences, action
plugins (`~/.config/dialstudio/actions`), an optional Waybar module
(`dialstudio status --waybar` → JSON with battery and profile), import/export
of a profile as a single `.json`.

**M4 / v0.4.0: release.**
README with screenshot (made-up content), AUR packages, and an
`omarchy-pkgs` proposal.

**M5 (optional research): key LEDs.**
The product has 7 backlit keys, but no LED protocol has been captured. Use the
`reverse-engineer-anything` skill (REA) on the **Ulanzi Studio** desktop
package (it's an Electron/JS app with a native BLE helper, downloadable from
https://www.ulanzi.com/pages/downloads). Run `analyze_javascript_application`
on its ASAR and search for the TEA key bytes / `0x1971` /
`getDeviceVersionMessage` neighbours (kriomant found the
`get…Message` builder names statically, so a light/colour builder is probably
next to them). Confirm any candidate command on the real dial with a variant
of `research/spike_bluez.py`, one write at a time, reading back the settings
afterwards. Only ship it if it's verified on hardware.

---

## 9. Testing rules

- Unit-test codec, parser, inputstate (feed synthetic event sequences: chord,
  long-press, press-rotate suppressing click, coalescing), config validation and
  migration, and action argv building (assert the argv, don't spawn).
- `bluezlink` is tested against a **fake BlueZ** on a private D-Bus session bus
  (`dbus-run-session`). A small QtDBus object implementing ObjectManager +
  GattCharacteristic1 that records writes and can emit notifies. Assert that
  the heartbeat cadence is 1 s ±100 ms and that it reconnects after
  `Connected=false`.
- Hardware checks are manual and listed in `docs/hardware-test.md`, with
  results recorded per release. Never claim a hardware behaviour was tested
  unless it ran on the dial.
- `bin/test` runs offscreen (`QT_QPA_PLATFORM=offscreen`) and must not touch
  the real `~/.config` or system bus.

## 10. Pitfalls (read these)

1. **Heartbeat is mandatory.** No heartbeat means no vendor events, and the
   dial keeps sending factory volume/media/Ctrl-C keys *as well as* your
   actions. If you see double actions, the heartbeat isn't running.
2. The knob click only ever arrives on release, as a down/up pair 1 ms apart.
   Don't implement knob long-press. It's physically impossible to detect.
3. IDs `41`–`44` are offline-mode IDs. If they show up, online mode lapsed.
   Log a warning and resend the heartbeat right away.
4. Don't hard-code `/org/bluez/hci0/...char004c` paths. Resolve by UUID; the
   adapter may be `hci1`.
5. The battery `0b 7b` report and heartbeat acks come on the same notify char
   as the events. Parse by prefix and don't treat unknown messages as errors.
6. Serial fragments can be duplicated. Dedupe by index.
7. Don't run actions in the D-Bus callback thread synchronously. Queue them.
8. `wtype` needs the virtual-keyboard protocol, which Hyprland supports. Keys
   go to the focused window, which is expected.
9. Ulanzi Studio isn't on Linux, so there's no conflict. But if the user also
   pairs the dial to a Mac/Windows host running Studio, only the active host
   (set by the switch underneath) gets events.
10. Commit messages are short and imperative. Make separate commits for docs,
    structure and tests.

## 10a. Ideas adopted from racerxdl/ulanzi-d200-linux (D200 USB deck, Python + web UI)

Not protocol-relevant (different device and transport), but its product design
is worth copying:

- **"Open application" picker.** List visible `.desktop` entries from XDG dirs
  (`/usr/share/applications`, `~/.local/share/applications`, Flatpak exports),
  with localized names. Store the stable `.desktop` ID, not a command string.
  Launch with `gtk-launch <id>` or `uwsm-app -- <id>` so Exec arguments and
  field codes are handled. Show the app's icon on the key in the dial view.
- **Detach launched apps from the daemon.** Run `exec`/`app` actions with
  `systemd-run --user --scope --collect -- ...`, so restarting dialstudio
  (or a service upgrade) doesn't kill the apps a button opened. Fall back to
  `startDetached` if `systemd-run` fails.
- **Saved layouts / presets.** Named profile snapshots in
  `~/.config/dialstudio/presets/*.json` with Save / Load / Delete in the GUI.
  Loading only opens the preset in the editor; "Apply" makes it active.
- **Default config is self-contained** and creates itself atomically on first
  start without overwriting an existing file (already in §4).
- **Reapply on reconnect.** The D200 daemon restores state after a reconnect.
  For us the equivalent is re-sending the heartbeat plus the haptics and sleep
  settings from config after every reconnect.
- **Do not copy:** its web UI (we use native Qt per Omarchy convention), its
  Ulanzi logo in the desktop icon (trademark; ours is an original SVG), or its
  `pip install -e .` install path.

## 11. References

- D200 deck manager (UX ideas only): https://github.com/racerxdl/ulanzi-d200-linux

- Protocol (BLE, TEA, events, settings): https://github.com/kriomant/ulanzi-d100h (copy in `research/`)
- HID/Studio behaviour, dial viewer idea, specs: https://github.com/brendanwelsh/ulanzi-d100h-homebrew
- Daemon/config/service/setup patterns (D200X, Python, Niri): https://github.com/doitian/ulanzi-studio-niri
- Omarchy app conventions: the `omarchy-app` skill (Hype / Omacut / Monologue repos)
- Verified Linux spike: `research/spike_bluez.py`
