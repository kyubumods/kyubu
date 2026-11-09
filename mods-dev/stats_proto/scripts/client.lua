-- stats_proto, client side (task 4.3 of add-ability-api, probe ability-stat-detours). The ability has no callbacks: the step
-- runner (task 4.4) does not exist yet, so the probe looks only at what the game asks the stat functions. `stats_proto:state`
-- prints the player's mode and mp and the game's answers (ability_cost, ability_cooldown at levels -1, 0, 1 and 9, canUseAbility)
-- for the mod's number, the placeholder (111) and smash (54), and modeDuration for the player's current mode.
local ffi = require("ffi")

local GC_PLAYER = 0x8006C8
local P_MODE, P_TIME, P_MP, P_CLASS, P_COOLDOWNS = 0x68, 0x6C, 0x170, 0x140, 0x139C

local ID = "stats_proto:burst"
-- definitions are allowed in the register phase only: the scripts load before it starts
-- (a loader before task 4.1 has no kyubu.ability: the probe then only reads smash's values, the baseline of "Smash unchanged")
local function number_of() return kyubu.ability and kyubu.ability.number(ID) or nil end
kyubu.on("lifecycle.register", function()
  if not kyubu.ability then kyubu.info("stats_proto: this loader has no kyubu.ability"); return end
  local ok, err = kyubu.ability.define(ID, {
    duration_ms = 800,
    mp_cost = 0.5,
    max_rank = 1,
    cooldown_ms = function(rank) return 4000 - 500 * rank end,   -- rank 0: 4000, rank 1: 3500
    claim = { slot = 4, class = "warrior", specialization = "any" },
  })
  kyubu.info(("stats_proto: define %s: %s"):format(ID, ok and "ok" or tostring(err)))
end)

local function va(address) return kyubu.rva_to_addr(address - 0x400000) end
local can_use = ffi.cast("bool(__thiscall*)(void*, int)", va(0x43EE40))
local cost_of = ffi.cast("float(__thiscall*)(void*, int, int)", va(0x4450E0))
local cooldown_of = ffi.cast("int(__thiscall*)(void*, int, int)", va(0x43EF40))
local mode_duration = ffi.cast("int(__thiscall*)(void*)", va(0x43DA30))

local function u32(addr) return ffi.cast("uint32_t*", addr)[0] end
local function rd32(addr)
  local bytes = kyubu.read(addr, 4)
  return bytes and ffi.cast("uint32_t*", bytes)[0] or nil
end
local function player()
  local p = kyubu.symbol("cube::game_controller")
  local gc = p and u32(p) or 0
  return gc ~= 0 and u32(gc + GC_PLAYER) or 0
end

-- The cooldown map (std::map<int, ...> at Creature+0x139C: head pointer, then size; a node holds left, parent, right, the colour
-- and isnil bytes, then the pair at +0x10): { [id] = value as an int }
local function cooldown_entries(p)
  local head = rd32(p + P_COOLDOWNS)
  local out = {}
  if not head then return out, 0 end
  local stack, count = { rd32(head + 4) }, 0
  while #stack > 0 and count < 64 do
    local n = table.remove(stack)
    local node = n and kyubu.read(n, 0x18)
    if n and n ~= head and node and node:byte(14) == 0 then
      count = count + 1
      out[ffi.cast("int32_t*", node:sub(17, 20))[0]] = ffi.cast("int32_t*", node:sub(21, 24))[0]
      stack[#stack + 1] = ffi.cast("uint32_t*", node:sub(1, 4))[0]
      stack[#stack + 1] = ffi.cast("uint32_t*", node:sub(9, 12))[0]
    end
  end
  return out, rd32(p + P_COOLDOWNS + 4) or 0
end

local function say(print, text) kyubu.info(text); print(text) end

local function answers(self, label, n)
  return ("%s %d: can_use %s cost %.4f cooldown(-1) %d (0) %d (1) %d (9) %d"):format(label, n, tostring(can_use(self, n)),
    cost_of(self, n, -1), cooldown_of(self, n, -1), cooldown_of(self, n, 0), cooldown_of(self, n, 1), cooldown_of(self, n, 9))
end

kyubu.command("stats_proto:state", "stats_proto:state: mode, mp and the game's answers for the mod's ability, the placeholder and smash", function(_, print)
  local p = player()
  if p == 0 then print("stats_proto: no player"); return end
  local self = ffi.cast("void*", p)
  local number = number_of()
  say(print, ("stats_proto: class %d mode %d time %d mp %.3f modeDuration(now) %d"):format(ffi.cast("uint8_t*", p + P_CLASS)[0],
    ffi.cast("uint8_t*", p + P_MODE)[0], tonumber(u32(p + P_TIME)), ffi.cast("float*", p + P_MP)[0], mode_duration(self)))
  if number then say(print, "stats_proto: " .. answers(self, "mod", number)) end
  say(print, "stats_proto: " .. answers(self, "placeholder", 111))
  say(print, "stats_proto: " .. answers(self, "smash", 54))
end)

kyubu.command("stats_proto:cooldowns", "stats_proto:cooldowns: the player's cooldown map", function(_, print)
  local p = player()
  if p == 0 then print("stats_proto: no player"); return end
  local entries, size = cooldown_entries(p)
  local out = {}
  for id, v in pairs(entries) do out[#out + 1] = ("%d=%d"):format(id, v) end
  table.sort(out)
  say(print, ("stats_proto: cooldown map size %d: %s"):format(size, table.concat(out, " ")))
end)

kyubu.command("stats_proto:mp", "stats_proto:mp <x>: sets the player's mp", function(args, print)
  local p, x = player(), tonumber(args)
  if p == 0 or not x then print("stats_proto: usage: mp <x> (needs a player)"); return end
  ffi.cast("float*", p + P_MP)[0] = x
  say(print, ("stats_proto: mp set to %.3f"):format(x))
end)

-- What the game does each frame, logged only when it changes: the player's mode (with the mode time and mp; mp falls by itself,
-- so it is not a trigger), canUseAbility
-- for the mod's number, and the cooldown-map entries of the mod's number, the placeholder and smash. The hook is the one of
-- task 2.1 (cube::GameController::abilityBarBuiltSite, 0x48BFDC: [esp+0x10] is the game controller); it only reads.
local seen = { mode = -1, can = "", cd = "" }
local function observe(p)
  local mode = ffi.cast("uint8_t*", p + P_MODE)[0]
  local mp = ffi.cast("float*", p + P_MP)[0]
  local number = number_of()
  local can = number and tostring(can_use(ffi.cast("void*", p), number)) or "-"
  local entries = cooldown_entries(p)
  local cd = ("112/n=%s 111=%s 54=%s"):format(tostring(number and entries[number]), tostring(entries[111]), tostring(entries[54]))
  local mp_text = ("%.3f"):format(mp)
  if mode ~= seen.mode then
    kyubu.info(("stats_proto: mode %d -> %d (mode time %d ms, mp %s, modeDuration %d)"):format(seen.mode, mode,
      tonumber(u32(p + P_TIME)), mp_text, mode_duration(ffi.cast("void*", p))))
    seen.mode = mode
  end
  if can ~= seen.can then kyubu.info(("stats_proto: canUseAbility(%s) = %s (mode %d, mp %s)"):format(tostring(number), can, mode, mp_text)); seen.can = can end
  if cd ~= seen.cd then kyubu.info(("stats_proto: cooldown entries %s (mode %d)"):format(cd, mode)); seen.cd = cd end
end
local hook_id, err = kyubu.hook_mid(0x48BFDC, function(ctx)
  local gc = u32(ctx.esp + 0x10)
  local p = gc ~= 0 and u32(gc + GC_PLAYER) or 0
  if p == 0 then return end
  local ok, e = pcall(observe, p)
  if not ok and not seen.failed then seen.failed = true; kyubu.info("stats_proto: observe failed: " .. tostring(e)) end
end)
kyubu.info(("stats_proto: observer at 0x48BFDC %s"):format(hook_id and "installed" or ("FAILED: " .. tostring(err))))
