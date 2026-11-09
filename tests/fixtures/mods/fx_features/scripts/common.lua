if kyubu.side == "client" then
  -- custom registry owned by this mod (PreInit), filled during Register
  assert(kyubu.registry.create("fx_features:spells", 1, true))
end
kyubu.on("lifecycle.register", function()
  kyubu.registry.add("fx_features:spells", "fx_features:frostnova", "{}")
  kyubu.model.add("fx_features:proc", "proc-tree7.cub")
  kyubu.model.remap("cubeworld:iron-sword1", "fx-remap-sword.cub")
end)
kyubu.on("lifecycle.post_register", function()
  kyubu.info("spells: " .. tostring(kyubu.registry.count("fx_features:spells")) ..
             ", proc slot " .. tostring(kyubu.model.slot("fx_features:proc")) ..
             ", hash " .. tostring(kyubu.registry.hash("kyubu:models")))
end)
