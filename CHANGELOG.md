# Changelog

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
