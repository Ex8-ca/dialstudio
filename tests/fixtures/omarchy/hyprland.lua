-- Fixture for the Omarchy shortcut scanner: a miniature of Omarchy's helpers plus a
-- user override. It is evaluated against the scanner's stub `hl`, never by Hyprland.
o = {}
o.bind_commands = {}

function o.bind(keys, description, dispatcher, options)
  local opts = options or {}
  if description then opts.description = description end
  if type(dispatcher) == "table" and dispatcher.launch then
    dispatcher = "uwsm-app -- " .. dispatcher.launch
  end
  if type(dispatcher) == "string" then dispatcher = hl.dsp.exec_cmd(dispatcher) end
  hl.bind(keys, dispatcher, opts)
end

local function copier()
  local fn = function() end
  return fn
end

o.bind("SUPER + RETURN", "Terminal", "omarchy-launch-terminal")
o.bind("XF86AudioRaiseVolume", "Volume up", hl.dsp.global("omarchy:audio.raise"), { locked = true })
o.bind("SUPER + SHIFT + F", "File manager", "nautilus")
o.bind("SUPER + W", "Close window", hl.dsp.window.close())
o.bind("SUPER + Q", "Close window", hl.dsp.window.close())
o.bind("SUPER + C", "Universal copy", copier())
o.bind("SUPER + T", "Toggle window floating/tiling", hl.dsp.window.float({ action = "toggle" }))
o.bind("SUPER + ALT + RETURN", "Tmux", "omarchy-launch-terminal-tmux --flag value")
o.bind("SUPER + SHIFT + A", nil, "no-description-is-skipped")
o.bind("SUPER + F1", "Pipes and quoting", "echo \"$(date)\" | tee /tmp/x")
o.bind("SUPER + F2", "Dangerous lua", hl.dsp.exec_cmd and "x" and os.getenv)
o.bind("SUPER + CTRL + ALT + SHIFT + comma", "All modifiers", "omarchy-all-mods")
o.bind("ALT + TAB", "Reveal active window on top", hl.dsp.window.bring_to_top())
o.bind("ALT + SHIFT + TAB", "Reveal active window on top", hl.dsp.window.alter_zorder({ mode = "bottom" }))

for workspace = 1, 3 do
  local key = "code:" .. tostring(workspace + 9)
  o.bind("SUPER + " .. key, "Switch to workspace " .. workspace, hl.dsp.focus({ workspace = tostring(workspace) }))
end

-- A personal override: unbinding a chord removes the default that sat on it.
hl.unbind("SUPER + SHIFT + F")
o.bind("SUPER + SHIFT + F", "File manager", { launch = "/home/u/.local/bin/strata" })

-- A bind with a tab in its description must survive the scanner's tab-separated output.
o.bind("SUPER + F3", "Tab\tin name", "omarchy-tab")
