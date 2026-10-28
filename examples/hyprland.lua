-- hypr-window-rotate: load the plugin (adjust the path to your package or install prefix)
--   Arch / Debian / Ubuntu: /usr/lib/hyprland/plugins/hypr-window-rotate.so
--   Fedora:                 /usr/lib64/hyprland/hypr-window-rotate.so
--   make install PREFIX=~/.local: ~/.local/lib/hyprland/plugins/hypr-window-rotate.so
hl.plugin.load("/usr/lib/hyprland/plugins/hypr-window-rotate.so")

-- SUPER + SHIFT + CTRL + ALT + arrow: the arrow is where the window's top edge points
local mods = "SUPER + SHIFT + CTRL + ALT"
hl.bind(mods .. " + up",    function() hl.plugin.window_rotate.rotate(0) end)
hl.bind(mods .. " + right", function() hl.plugin.window_rotate.rotate(90) end)
hl.bind(mods .. " + down",  function() hl.plugin.window_rotate.rotate(180) end)
hl.bind(mods .. " + left",  function() hl.plugin.window_rotate.rotate(270) end)
