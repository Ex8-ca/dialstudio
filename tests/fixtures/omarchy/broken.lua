o = { bind = function(keys, description, command) hl.bind(keys, hl.dsp.exec_cmd(command), { description = description }) end }
o.bind("SUPER + A", "Before the error", "omarchy-first")
error("config blew up half way")
