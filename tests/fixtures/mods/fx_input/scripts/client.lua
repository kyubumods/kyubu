local enters, clicks = 0, 0
kyubu.on("input.key", function(ev)
  if ev.vk == 13 then
    ev:cancel()
    if ev.down then enters = enters + 1; kyubu.info("cancelled Enter #" .. enters) end
  end
end)
kyubu.on("input.mouse_button", function(ev)
  if kyubu.ui.want_input() then
    ev:cancel()
    if ev.down then clicks = clicks + 1; kyubu.info("click consumed by the overlay #" .. clicks) end
  end
end)
kyubu.on("render.overlay", function()
  if kyubu.ui.begin("fx_input") then
    kyubu.ui.text(("Enter cancelled %d times, clicks consumed %d"):format(enters, clicks))
  end
  kyubu.ui["end"]()
end)
