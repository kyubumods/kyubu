-- Fixture for the Mods page settings form (add-mod-manager 6.3 and 6.4): logs each option change.
kyubu.on("config.changed", function(ev)
  if ev.mod == kyubu.id then
    kyubu.info("mm_fixture_opts: config.changed " .. ev.key .. " = " .. tostring(kyubu.config.get(ev.key)))
  end
end)
