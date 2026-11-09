local show = true
local speed = tonumber(kyubu.config.get("speed")) or 1.0   -- declared in config.toml
local pongs = 0
local frames = 0

kyubu.on("frame.begin", function(ev) frames = ev.frame end)

kyubu.on("render.overlay", function()
  if not show then return end
  local visible, open = kyubu.ui.begin("Hello Lua", true)
  if visible then
    kyubu.ui.text(kyubu.text.get("hello_lua:greeting") or "?")
    kyubu.ui.text(("frame %d, pongs from the server side: %d"):format(frames, pongs))
    local _, v = kyubu.ui.slider_float("speed", speed, 0, 5)
    speed = v
    kyubu.ui.same_line()
    if kyubu.ui.button("save") then kyubu.config.set("speed", speed) end
    if kyubu.ui.button("ping server") then
      kyubu.channel.send("hello_lua:ping", kyubu.channel.SERVER, { n = frames, msg = "ping" })
    end
  end
  kyubu.ui.finish()
  show = open
end)

-- the option changed: from the console (config hello_lua speed 2), a reload of the file, or the save button
kyubu.on("config.changed", function(ev)
  if ev.mod == kyubu.id and ev.key == "speed" then
    speed = tonumber(kyubu.config.get("speed")) or speed
    kyubu.info("speed is now " .. tostring(speed))
  end
end)

kyubu.channel.register("hello_lua:pong", function(conn, msg)
  pongs = pongs + 1
  kyubu.info("pong #" .. pongs .. " for frame " .. tostring(msg.n))
end)

kyubu.command("hello_lua:toggle", "show or hide the Hello Lua window", function(args, print)
  show = not show
  print("Hello Lua window " .. (show and "shown" or "hidden"))
end)

-- F9 toggles the window too; the key never reaches the game.
kyubu.on("input.key", function(ev)
  if ev.vk == 0x78 and ev.down then
    show = not show
    ev:cancel()
  end
end)
