-- Client side: UI, input, console commands.
local open = kyubu.config.get("show_window") ~= false   -- an option from config.toml
local clicks = 0

kyubu.on("render.overlay", function()
  if not open then return end
  local visible, still_open = kyubu.ui.begin("My Lua Mod", true)
  if visible then
    kyubu.ui.text(kyubu.text.get("my_lua_mod:hello") or "")
    if kyubu.ui.button("ask the server") then
      kyubu.channel.send("my_lua_mod:ask", kyubu.channel.SERVER, { clicks = clicks })
    end
    kyubu.ui.text("answers: " .. clicks)
  end
  kyubu.ui.finish()
  open = still_open
end)

kyubu.channel.register("my_lua_mod:answer", function(_, msg)
  clicks = msg.clicks
end)

kyubu.command("my_lua_mod:show", "show the window again", function(_, print)
  open = true
  print("window shown")
end)
