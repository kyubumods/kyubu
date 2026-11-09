-- equip_gate_probe, client side: `equip_gate_probe:check` also calls creature_power on the same kind of stand-in creature, and calls can_equip_item (a thiscall on the creature with the item as its
-- one stack argument) for a list of (creature level, item level) pairs and logs each result beside the answer that the power
-- rule gives. The creatures are zeroed stand-ins that hold only what the function reads (the level at +0x190 and the class
-- byte at +0x140); the item is a copy of the player's own weapon with its level (+0x10) changed. The stand-ins keep the real
-- player out of the check, so no state of the game changes.
local ffi = require("ffi")
local cw = require("cubeworld")

local WARRIOR = 1
local ITEM_SIZE = 0x118
local WEAPON_SLOTS = { 0x990, 0xAA8 }   -- equipment[6] and [7], the player's two weapon slots (Creature offsets)

-- the game's power of a level: int(100 * (1 - 1/(0.05 * (level - 1) + 1)) + 1), the conversion truncating toward zero
local function power(level)
  local v = 100 * (1 - 1 / (0.05 * (level - 1) + 1)) + 1
  return v >= 0 and math.floor(v) or math.ceil(v)
end

-- creature level, item level
local CASES = {
  { 0, 1 }, { 1, 1 }, { 21, 1 }, { 21, 21 }, { 21, 22 }, { 1, 2 }, { 100, 1981 }, { 1981, 1981 },
}

local POWER_LEVELS = { 0, 1, 2, 21, 22, 51, 100, 981, 1981 }

kyubu.command("equip_gate_probe:check", "equip_gate_probe:check: call can_equip_item on stand-in creatures and the player's weapon", function(_, print)
  local function say(text) kyubu.info(text); print(text) end
  local addr = kyubu.symbol("can_equip_item")
  local gc_addr = kyubu.symbol("cube::game_controller")
  if not addr or not gc_addr then say("equip_gate_probe: symbols missing (can_equip_item, cube::game_controller)"); return end
  local gate = ffi.cast(cw.functions["can_equip_item"], addr)
  local gc = ffi.cast("uintptr_t*", gc_addr)[0]
  local player = ffi.cast("uintptr_t*", gc + 0x8006C8)[0]
  if player == 0 then say("equip_gate_probe: no player yet (enter a world first)"); return end
  local weapon
  for _, off in ipairs(WEAPON_SLOTS) do
    if ffi.cast("uint8_t*", player + off)[0] ~= 0 then weapon = player + off; break end
  end
  if not weapon then say("equip_gate_probe: the player holds no weapon"); return end
  local item = ffi.new("cube_ItemData[1]")
  ffi.copy(item, ffi.cast("void*", weapon), ITEM_SIZE)
  say(("equip_gate_probe: weapon type %d sub_type %d, stored level %d; creature class %d"):format(
    item[0].type, item[0].sub_type, item[0].level, WARRIOR))
  local bad = 0
  for i, c in ipairs(CASES) do
    local creature_level, item_level = c[1], c[2]
    local stand_in = ffi.new("uint8_t[0x1300]")
    ffi.cast("int32_t*", stand_in + 0x190)[0] = creature_level
    stand_in[0x140] = WARRIOR
    item[0].level = item_level
    local got = gate(ffi.cast("cube_Creature*", stand_in), nil, item)
    local want = power(creature_level) >= power(item_level)
    local ok = got == want
    if not ok then bad = bad + 1 end
    say(("equip_gate_probe: case %d: creature level %d (power %d), item level %d (power %d): can_equip_item = %s, the power rule says %s%s"):format(
      i, creature_level, power(creature_level), item_level, power(item_level), tostring(got), tostring(want), ok and "" or "  MISMATCH"))
  end
  say(("equip_gate_probe: %d case(s), %d mismatch(es)"):format(#CASES, bad))

  -- creature_power (a __fastcall with the creature in ECX and no stack argument): the power of the creature's level
  local power_of = ffi.cast(cw.functions["creature_power"], assert(kyubu.symbol("creature_power")))
  local bad_power = 0
  for _, level in ipairs(POWER_LEVELS) do
    local stand_in = ffi.new("uint8_t[0x1300]")
    ffi.cast("int32_t*", stand_in + 0x190)[0] = level
    local got = power_of(ffi.cast("cube_Creature*", stand_in))
    local want = power(level)
    if got ~= want then bad_power = bad_power + 1 end
    say(("equip_gate_probe: creature_power(level %d) = %d, the power rule says %d%s"):format(
      level, got, want, got == want and "" or "  MISMATCH"))
  end
  say(("equip_gate_probe: %d creature_power level(s), %d mismatch(es)"):format(#POWER_LEVELS, bad_power))
end)

-- Task 4.2 of add-level-rework (probe level-strength): `equip_gate_probe:strength` calls creature_power for the levels 1 to 100 and the
-- game's maxHp for a few levels, all on stand-in creatures (zeroed for creature_power; a copy of the player with another level for maxHp), and
-- logs every value. With level_rework loaded, creature_power must return N for every N, and maxHp must follow the vanilla max HP of
-- the level old(N); a vanilla run (no mod) of the same command prints the reference values to compare with.
local MAXHP_LEVELS = { 1, 2, 3, 21, 22, 51, 100, 981, 1981 }

kyubu.command("equip_gate_probe:strength", "equip_gate_probe:strength: creature_power for levels 1-100 and maxHp for chosen levels, on stand-in creatures", function(_, print)
  local function say(text) kyubu.info(text); print(text) end
  local power_of = ffi.cast(cw.functions["creature_power"], assert(kyubu.symbol("creature_power")))
  local max_hp = ffi.cast(cw.functions["cube::Creature::maxHp"], assert(kyubu.symbol("cube::Creature::maxHp")))
  -- A zeroed stand-in has max_hp_multiplier 0, so its maxHp would be 0: this one is a copy of the player's creature (its class,
  -- multipliers and worn gear, the same in every run) with only the level changed.
  local gc = ffi.cast("uintptr_t*", assert(kyubu.symbol("cube::game_controller")))[0]
  local player = ffi.cast("uintptr_t*", gc + 0x8006C8)[0]
  if player == 0 then say("strength_probe: no player yet (enter a world first)"); return end
  local function stand_in(level)
    local c = ffi.new("uint8_t[0x1300]")
    ffi.copy(c, ffi.cast("void*", player), 0x1300)
    ffi.cast("int32_t*", c + 0x190)[0] = level
    c[0x140] = WARRIOR
    return c
  end
  say(("strength_probe: the player's level is %d"):format(ffi.cast("int32_t*", player + 0x190)[0]))
  local bad = 0
  for n = 1, 100 do
    local c = stand_in(n)
    local got = power_of(ffi.cast("cube_Creature*", c))
    if got ~= n then bad = bad + 1; say(("strength_probe: creature_power(level %d) = %d  MISMATCH"):format(n, got)) end
  end
  say(("strength_probe: creature_power: levels 1 to 100, %d mismatch(es)"):format(bad))
  for _, level in ipairs(MAXHP_LEVELS) do
    local c = stand_in(level)
    local hp = max_hp(ffi.cast("cube_Creature*", c), nil)
    say(("strength_probe: maxHp(level %d) = %.6f"):format(level, hp))
  end
end)

-- Task 4.5 of add-level-rework (probe pet-levels): `equip_gate_probe:pets` calls can_equip_item with a pet item (type 0x13, the stored level at +0x10) on
-- stand-in creatures of chosen levels, and logs each result beside two answers: the mod's rule (the pet's level, clamped to 1-100, at most the
-- creature's level, and the class rule) and vanilla's (power of the pet's stored level at most power of the creature's level). Cases where they
-- differ, such as a level-99 character and a level-100 pet (both power 84) or a level-100 character and a level-600 pet, show which one the game
-- follows. The class rule is the game's own item_usable_by_class, called on the same item.
local PET_CASES = {
  -- creature level, pet stored level
  { 50, 50 }, { 50, 51 }, { 50, 49 }, { 1, 1 }, { 1, 2 }, { 99, 100 }, { 100, 100 }, { 100, 600 }, { 99, 600 }, { 100, 1981 },
}

kyubu.command("equip_gate_probe:pets", "equip_gate_probe:pets: call can_equip_item with pet items on stand-in creatures", function(_, print)
  local function say(text) kyubu.info(text); print(text) end
  local gate_addr = kyubu.symbol("can_equip_item")
  local usable_addr = kyubu.symbol("item_usable_by_class")
  if not gate_addr or not usable_addr then say("equip_gate_probe: symbols missing (can_equip_item, item_usable_by_class)"); return end
  local gate = ffi.cast(cw.functions["can_equip_item"], gate_addr)
  local usable = ffi.cast(cw.functions["item_usable_by_class"], usable_addr)
  local item = ffi.new("cube_ItemData[1]")
  item[0].type = 0x13
  local bad, differ = 0, 0
  for i, c in ipairs(PET_CASES) do
    local creature_level, stored = c[1], c[2]
    local stand_in = ffi.new("uint8_t[0x1300]")
    ffi.cast("int32_t*", stand_in + 0x190)[0] = creature_level
    stand_in[0x140] = WARRIOR
    item[0].level = stored
    local got = gate(ffi.cast("cube_Creature*", stand_in), nil, item)
    local class_ok = usable(item, nil, WARRIOR)
    local mod_level = math.max(1, math.min(100, stored))
    local mod_rule = mod_level <= creature_level and class_ok
    local vanilla_rule = power(creature_level) >= power(stored) and class_ok
    if got ~= mod_rule then bad = bad + 1 end
    if mod_rule ~= vanilla_rule then differ = differ + 1 end
    say(("equip_gate_probe: pet case %d: creature level %d, pet stored level %d (mod level %d): can_equip_item = %s, the mod's rule says %s, vanilla's says %s (class rule %s)%s%s"):format(
      i, creature_level, stored, mod_level, tostring(got), tostring(mod_rule), tostring(vanilla_rule), tostring(class_ok),
      mod_rule ~= vanilla_rule and "  [the rules differ]" or "", got == mod_rule and "" or "  MISMATCH"))
  end
  say(("equip_gate_probe: %d pet case(s), %d where the rules differ, %d mismatch(es) with the mod's rule"):format(#PET_CASES, differ, bad))
end)

-- Task 6.2 of add-level-rework (probe item-levels): `equip_gate_probe:items` calls can_equip_item with a copy of the player's weapon (stored level
-- changed) on stand-in warriors, and logs each result beside the mod's rule (the item's power, 1 to 100, at most the character's level,
-- and the class rule) and vanilla's (power against power).
local ITEM_CASES = {
  -- creature level, item stored level
  { 50, 50 },    -- "Above the character's level": power 72 against level 50
  { 50, 20 },    -- "At the character's level": power 49 against level 50
  { 50, 21 }, { 51, 21 }, { 1, 1 }, { 1, 2 }, { 100, 1981 }, { 99, 1981 }, { 100, 600 }, { 99, 100 },
}

kyubu.command("equip_gate_probe:items", "equip_gate_probe:items: call can_equip_item with the player's weapon at other stored levels on stand-in creatures", function(_, print)
  local function say(text) kyubu.info(text); print(text) end
  local gate_addr = kyubu.symbol("can_equip_item")
  local gc_addr = kyubu.symbol("cube::game_controller")
  if not gate_addr or not gc_addr then say("equip_gate_probe: symbols missing (can_equip_item, cube::game_controller)"); return end
  local gate = ffi.cast(cw.functions["can_equip_item"], gate_addr)
  local gc = ffi.cast("uintptr_t*", gc_addr)[0]
  local player = ffi.cast("uintptr_t*", gc + 0x8006C8)[0]
  if player == 0 then say("equip_gate_probe: no player yet (enter a world first)"); return end
  local weapon
  for _, off in ipairs(WEAPON_SLOTS) do
    if ffi.cast("uint8_t*", player + off)[0] ~= 0 then weapon = player + off; break end
  end
  if not weapon then say("equip_gate_probe: the player holds no weapon"); return end
  local item = ffi.new("cube_ItemData[1]")
  ffi.copy(item, ffi.cast("void*", weapon), ITEM_SIZE)
  say(("equip_gate_probe: weapon type %d sub_type %d, stored level %d; creature class %d"):format(item[0].type, item[0].sub_type, item[0].level, WARRIOR))
  local bad, differ = 0, 0
  for i, c in ipairs(ITEM_CASES) do
    local creature_level, stored = c[1], c[2]
    local stand_in = ffi.new("uint8_t[0x1300]")
    ffi.cast("int32_t*", stand_in + 0x190)[0] = creature_level
    stand_in[0x140] = WARRIOR
    item[0].level = stored
    local got = gate(ffi.cast("cube_Creature*", stand_in), nil, item)
    local item_power = math.max(1, math.min(100, power(stored)))
    local mod_rule = item_power <= math.max(1, math.min(100, creature_level))
    local vanilla_rule = power(creature_level) >= power(stored)
    if got ~= mod_rule then bad = bad + 1 end
    if mod_rule ~= vanilla_rule then differ = differ + 1 end
    say(("equip_gate_probe: item case %d: creature level %d, item stored level %d (power %d): can_equip_item = %s, the mod's rule says %s, vanilla's says %s%s%s"):format(
      i, creature_level, stored, item_power, tostring(got), tostring(mod_rule), tostring(vanilla_rule),
      mod_rule ~= vanilla_rule and "  [the rules differ]" or "", got == mod_rule and "" or "  MISMATCH"))
  end
  say(("equip_gate_probe: %d item case(s), %d where the rules differ, %d mismatch(es) with the mod's rule"):format(#ITEM_CASES, differ, bad))
end)
