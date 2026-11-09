-- Runs on every side the mod declares (twice in single-player: once per Lua VM).
kyubu.info("my_lua_mod loaded on the " .. kyubu.side .. " side")

kyubu.on("lifecycle.register", function()
  -- Registries are shared by both sides; registering the same entry on each side is fine.
  kyubu.text.add("my_lua_mod:hello", "Hello!", "Hallo!")
end)
