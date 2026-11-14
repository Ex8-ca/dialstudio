# Hardware test log

Only things that ran on the real dial or the real desktop are marked as verified. Everything else
is covered by the fake-BlueZ tests only, or not tested.

## v0.1.0 hardware run on the real dial (2026-11-14, with Marc present)

Run against a throwaway config; the user's real config was not touched.

| Check | Result |
|---|---|
| Daemon reaches `online`; `status` shows firmware 1.3.8.1, battery 100% | verified |
| Heartbeat holds online mode | verified |
| Haptics write: off, read back off, restored to on | verified |
| Sleep write: 3600 s, read back, restored to 7200 s | verified |
| key1..key7 press/release events (key mapping matches the plan) | verified |
| Knob click, rotate cw/ccw (steps=1 per detent) | verified |
| Press-rotate cw/ccw, no knob click on release | verified |
| Factory volume returns after daemon stops (user confirmed) | verified, delay not timed |
| Profile match on focused window class; bound key fired a command | verified (HermesApp profile) |
| Reconnect after dial power cycle; keys work again without a daemon restart | verified; recovery time NOT measured, the 30 s target is unproven |

Side effects seen: with the default config a knob click mutes system audio, and each detent moves
the volume 2%.

Still untested: write-without-request fallback, `learn` interactively, install.sh end to end,
`bind --omarchy` triggering the Hyprland bind (see the open point above).

## v0.1.0 (2026-11-14), earlier run without a dial

### Dial availability

The dial (`CA:62:68:9B:1B:D3`, paired, bonded, trusted) was **not connectable during this session**.
`bluetoothctl info` reported `Connected: no`; `connect` hung and then answered
`org.bluez.Error.InProgress`, and the daemon's own `Connect()` calls timed out after 30 s. This
persisted across several attempts over about 30 minutes, including after `Device1.Disconnect`.
The cause is unknown. Likely: the dial was asleep, off, or switched to another host (it pairs with
three hosts), but a stuck pending connect in bluetoothd cannot be ruled out. Nobody was present to
wake it.

Consequence: **none of the five M1 acceptance items could be run on the dial.**

### What did run against the real system

| Check | Result |
|---|---|
| `dialstudio doctor` on the real system bus | bluez ok, dial found by the vendor-service UUID, paired and trusted, adapter powered, `fd00`/`fd01` resolved by UUID (`service004b/char004c`, `char004f`), tools detected, `connected` = warn |
| Daemon starts, binds the socket, finds the dial by UUID, tries to connect with backoff | yes (reports `connecting`/`disconnected`; never reached `online`) |
| SIGTERM with the dial unreachable: daemon exits and removes its socket | yes |
| `dialstudio status`, `bind`, `unbind`, `bindings`, `send`, `shortcuts`, `profile`, `page` and `set` errors, against a daemon with a throwaway config and socket | yes |
| Hot reload: valid external edit applied, broken edit rejected and previous config kept, error shown in `status` | yes (daemon on a temp config) |
| `set haptics off` with no dial | reports "not connected; applied on next connect", writes config only |
| `~/.config/dialstudio`, `~/.config/hypr` and systemd units | never created or modified |

### Not verified on the dial

1. `dialstudio events` printing key, knob click, `rotate cw steps=N`, `press_rotate`.
2. Knob turn changing volume through `wpctl`, and factory volume returning within about 3 s after the daemon stops.
3. Reconnect within 30 s after toggling the dial's Bluetooth.
4. `dialstudio set haptics off` read back from the dial by `status`, and sleep read/write/restore
   (the original values are haptics on, sleep 7200 s; nothing was written to the dial).
5. A focused window matched by a profile switching bindings. Window matching is covered by unit
   tests and by the Hyprland event parser against a real Unix socket. On the real desktop only the
   fallback profile was seen, because the focused window (`google-chrome`, then the Hermes
   desktop app) matched no rule. A rule matching the focused class was not exercised end to end.
6. Battery, firmware, serial from the dial in `status`.
7. Behaviour of heartbeat and notifications against real BlueZ with a connected dial: this is
   covered by `research/spike_bluez.py` (previous session) and by the fake-BlueZ tests, not by
   this code on hardware.

To run these, wake the dial, then:

```sh
systemctl --user start dialstudio      # or: dialstudio daemon
dialstudio events                      # press each key, turn and push the knob
dialstudio status --json
dialstudio set haptics off && dialstudio status | grep haptics && dialstudio set haptics on
dialstudio set sleep 3600 && dialstudio status | grep sleep && dialstudio set sleep 7200
systemctl --user stop dialstudio       # knob must act as factory volume within ~3 s
```

### Open point: do Hyprland binds fire from injected keys? (PLAN section 5a)

Tested on Hyprland 0.56.2 with temporary binds that were created with `hyprctl eval hl.bind(...)`
and removed with `hl.unbind(...)` afterwards (bind count back to 277; the user's config files were
not touched). The bind action was `touch <file>` in the scratch directory.

| Test | Result |
|---|---|
| Control: `hyprctl dispatch hl.dsp.exec_cmd('touch f')` | file created, so the action path works |
| Bind `F24`, then `wtype -k F24` | **bind did not fire** |
| Bind `F24`, then `hyprctl dispatch hl.dsp.send_shortcut({mods="", key="F24", window="activewindow"})` | **bind did not fire** |
| Bind `CTRL+ALT+F23` (shows in `hyprctl binds` as modmask 12), then `wtype -M ctrl -M alt -k F23 ...` | **bind did not fire** |
| Same bind, then `send_shortcut({mods="CTRL ALT", key="F23", window="activewindow"})` | **bind did not fire** |

Caveats: this is a negative result from a single machine and a single run each. It does not prove
that no injection method works. It is consistent with `send_shortcut` delivering to the client
window rather than through Hyprland's own bind matcher, but the cause was not investigated, and I
did not test `send_key_state`, `hl.dsp.pass`, or a real keyboard.

Consequence for the plan: the `--omarchy "description"` link mode **cannot be relied on to trigger
the Hyprland bind**. It will send the key combo to the active window, which only helps if the app
itself handles that combo. The plan's fallback applies: resolve the description to the
`omarchy-*` command the bind runs (look in `~/.local/share/omarchy/` and `~/.config/hypr/`) and run
that. Not implemented in v0.1.0; `bind --omarchy` is shipped as a combo link and documented as such.

### Hyprland 0.55+ finding

`hyprctl dispatch workspace +1` (the form in PLAN section 5) is invalid on this Hyprland:
`dispatch` takes a Lua expression, and `hyprctl keyword` refuses to run ("use eval"). The `hypr`
action therefore maps legacy dispatcher names to `hl.dsp.*` Lua, and `hypr_zoom` uses
`hyprctl eval 'hl.config({ cursor = { zoom_factor = N } })'`. The `hl.dsp.focus`, `exec_cmd` and
`send_shortcut` forms were run against the live compositor; `send_shortcut` reports "window not
found" for a missing target and "key not found" for a bad key, and accepts `"CTRL"`, `"CTRL SHIFT"`
and `"CTRL + SHIFT"` mod strings.
