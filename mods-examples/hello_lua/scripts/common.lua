-- Runs once per logical side (twice in single-player).
kyubu.info("hello from Lua on the " .. kyubu.side .. " side")

-- Registries are shared by both sides of a process. Registering the same entry on every side is harmless
-- and keeps a dedicated server's registries identical to the client's.
kyubu.on("lifecycle.register", function()
  kyubu.text.add("hello_lua:greeting", "Hello from Lua!", "Hallo aus Lua!")
end)

kyubu.on("lifecycle.world_enter", function(ev)
  kyubu.info("world entered: " .. tostring(ev.world))
end)
