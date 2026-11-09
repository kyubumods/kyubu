-- frost_spells: adds entries to another mod's registry. Entry ids use this mod's namespace;
-- the payload is any string (here a packed Lua table).

kyubu.on("lifecycle.register", function()
  kyubu.registry.add("spells_lib:spells", "frost_spells:frost_nova", kyubu.pack({ name = "Frost Nova", mana = 30 }))
  kyubu.registry.add("spells_lib:spells", "frost_spells:ice_lance", kyubu.pack({ name = "Ice Lance", mana = 12 }))
end)

kyubu.on("lifecycle.game_ready", function()
  local number = kyubu.registry.lookup("spells_lib:spells", "frost_spells:ice_lance")
  kyubu.info("frost_spells:ice_lance is spell number " .. tostring(number))
end)
