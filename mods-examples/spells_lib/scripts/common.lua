-- spells_lib: creates a registry other mods fill, then reads it back once it is frozen.

-- PreInit (top level): create the registry. Its name must be in this mod's namespace.
-- Registries are shared by both sides of a process; creating the same registry again from the
-- other side is allowed, which keeps the code the same for single-player and a dedicated server.
assert(kyubu.registry.create("spells_lib:spells", 1, true))

-- PostRegister: every mod has added its spells; numbers are assigned and stable.
kyubu.on("lifecycle.post_register", function()
  local n = kyubu.registry.count("spells_lib:spells") or 0
  for i = 0, n - 1 do
    local id, number = kyubu.registry.entry("spells_lib:spells", i)
    local payload = id and kyubu.registry.payload("spells_lib:spells", id)
    if payload then
      local spell = kyubu.unpack(payload)
      kyubu.info(("spell #%d %s: %s, %d mana"):format(number, id, spell.name, spell.mana))
    end
  end
end)
