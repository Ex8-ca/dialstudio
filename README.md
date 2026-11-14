# Dial Studio

A Linux controller for the **Ulanzi D100H** dial (7 keys, one push-rotate knob, Bluetooth LE only),
built for Omarchy. It takes the dial over from its factory volume/media behaviour and lets each
control run commands, Hyprland dispatches, keystrokes, volume, media and brightness.

Status: **v0.1.0 is headless** (daemon + CLI). The GUI editor is planned for v0.1.5 and v0.2.0; see `PLAN.md`.

If the daemon stops, the dial goes back to its factory volume/media behaviour within about 3 s.
The firmware does that, not this program.

## Install

On Omarchy or Arch:

```sh
git clone https://github.com/Ex8-ca/dialstudio && cd dialstudio && bin/install
systemctl --user enable --now dialstudio
dialstudio doctor
```

`bin/install` runs `makepkg -fsi`. `install.sh` does the same from a one-liner and is safe to re-run.
The dial must already be paired (Omarchy menu, Setup, Bluetooth). No udev rule or group change is
needed: BlueZ's D-Bus API works for the logged-in user.

Optional tools, detected at runtime: `playerctl`, `swayosd-client`, `brightnessctl`, `notify-send`, `uwsm-app`.

## Controls

| Name | Physical control | Wire ID |
|---|---|---|
| `key1` `key2` `key3` | top row, left to right | 01 02 03 |
| `key4` | upper-left side key | 04 |
| `key5` | upper-right side key | 05 |
| `key6` | lower-left side key | 06 |
| `key7` | lower-right side key | 08 |
| `knob.click` | knob push (fires on release) | 07 |
| `knob.rotate_cw` / `knob.rotate_ccw` | turn | 0a / 09 |
| `knob.press_rotate_cw` / `_ccw` | turn while pressed | 0c / 0b |

Key slots are `press` (default), `long_press` and `release`. A knob rotation bound with
`@keyN`, for example `knob.rotate_cw@key4`, applies while that key is held, and the key's own
`press` is then cancelled. Press-rotate without a binding falls through to plain rotate.
The knob has no long press: the dial only reports a click on release.

## Commands

```
dialstudio daemon                      run the headless daemon (the systemd unit does this)
dialstudio status [--json]             link, device, battery, firmware, haptics, sleep, profile
dialstudio events [--json]             stream decoded events: "key1 press", "rotate cw steps=3", ...
dialstudio set haptics on|off          write the dial's setting (reply is the dial's own answer)
dialstudio set sleep <seconds>         0 disables
dialstudio profile <name>|auto         pin a profile, or go back to window matching
dialstudio page next|prev|<n>
dialstudio doctor [--json]             bluez, pairing, characteristics, tools, config, unit

dialstudio bind <control> <combo> [--on press|long_press|release] [--profile NAME] [--page N] [--via wtype|hypr]
dialstudio bind <control> --omarchy "Volume up"
dialstudio bind <control> --action '{"type":"exec","command":"foot"}'
dialstudio unbind <control> [--on SLOT]
dialstudio bindings [--json] [--profile NAME]
dialstudio send <combo> [--via wtype|hypr]
dialstudio shortcuts [--json] [FILTER]
dialstudio learn <control>
```

Every `bind` validates the combo (key names are checked against xkb keysyms, so `ctrl+Retrun` is
rejected), saves `~/.config/dialstudio/config.json` atomically, and the running daemon reloads it.

### Shortcut delivery

- `wtype` (default): synthesises the key press to the focused window.
- `hypr`: `hyprctl dispatch hl.dsp.send_shortcut(...)`, optionally aimed at a window.
- `--omarchy "description"`: links to an existing Hyprland bind by its description. Hyprland 0.55+
  reports every bind with dispatcher `__lua`, so the bind cannot be run directly. Dial Studio stores
  the description plus the key combo, re-resolves it on every config reload, and delivers the combo.
  Whether Hyprland fires its *own* binds from injected keys is **not yet verified**; see
  `docs/hardware-test.md`.

## Config

`~/.config/dialstudio/config.json`, written with defaults on first start (never overwritten). The
defaults reproduce the factory behaviour: media keys on the top row, copy/paste/undo/redo on the
side keys, volume on the knob, brightness on press-rotate. A broken file is never overwritten: the
daemon refuses to start, or keeps the previous config on a hot reload, and says why in
`dialstudio status` and `dialstudio events`.

Profiles switch with the focused window (Hyprland socket2 `activewindow`, regex on class and
optionally title; first match wins, `"match": null` is the fallback). Action types: `volume`, `mic`,
`brightness`, `media`, `keys`, `text`, `hypr`, `hypr_zoom`, `omarchy`, `exec`, `url`, `shell`,
`page`, `profile`, `haptics`, `multi`, `noop`. `exec` never goes through a shell; only `shell` does.

Hyprland actions use Lua because Hyprland 0.55+ dropped the old dispatcher syntax:
`{"type":"hypr","dispatch":"workspace","arg":"+1"}` runs `hyprctl dispatch 'hl.dsp.focus({ workspace = "+1" })'`.
For anything else use `{"type":"hypr","lua":"hl.dsp...."}`.

## Develop

```sh
bin/build      # qmake6 + make into build/dialstudio
bin/test       # unit tests offscreen, inside a private D-Bus session
```

`bin/test` never touches `~/.config` or the system bus: BlueZ is faked on a private session bus.

## License

MIT. See `LICENSE`.
