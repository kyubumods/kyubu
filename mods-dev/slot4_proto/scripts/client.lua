-- slot4_proto, client side (task 2.1 of add-ability-api, probe ability-slot4). The game builds the player's skill bar each
-- frame in cube::GameController::update: five ids in a std::vector<int> at GameController+0x80080C, then a learned gate.
-- The hook at 0x48BFDC (right after the gate: `mov edi,[esp+0x10]`) finds the game controller at [esp+0x10] and the vector
-- at [esp+0x34] and, when the vector holds exactly those five ids, appends ID with the game's own push_back (0x5BE230,
-- a thiscall with the vector in ECX and a pointer to the int as its one stack argument). The game's loops then run over six entries:
-- index 5 reads key byte GameController+9 and the HUD widget "4".
local ffi = require("ffi")

local GC_BAR = 0x80080C
local GC_PLAYER = 0x8006C8
local P_MODE, P_TIME, P_MP, P_CLASS, P_SPEC, P_COOLDOWNS = 0x68, 0x6C, 0x170, 0x140, 0x141, 0x139C

local LEARN = true                   -- set skills[6..8] (Creature+0x1150) to 1 so the learned gate keeps the three class abilities
local ID = 115                       -- the id pushed; `slot4_proto:id <n>` changes it (0 stops the push)
local pushed, skipped, last_size = 0, 0, -1

-- rva_to_addr takes an RVA: the image base is 0x400000
local function va(address) return kyubu.rva_to_addr(address - 0x400000) end
local push_back = ffi.cast("void(__thiscall*)(void*, int*)", va(0x5BE230))
local can_use = ffi.cast("bool(__thiscall*)(void*, int)", va(0x43EE40))
local cost_of = ffi.cast("float(__thiscall*)(void*, int, int)", va(0x4450E0))
local cooldown_of = ffi.cast("int(__thiscall*)(void*, int, int)", va(0x43EF40))

local function u32(addr) return ffi.cast("uint32_t*", addr)[0] end
-- guarded read of one 32-bit value (nil for a bad address), for walking the game's std::map
local function rd32(addr)
  local bytes = kyubu.read(addr, 4)
  return bytes and ffi.cast("uint32_t*", bytes)[0] or nil
end
local function game_controller()
  local p = kyubu.symbol("cube::game_controller")
  if not p then return 0 end
  return u32(p)
end
local function player_of(gc) return gc ~= 0 and u32(gc + GC_PLAYER) or 0 end
local function bar_of(gc)
  local out, vec = {}, gc + GC_BAR
  local b, e = u32(vec), u32(vec + 4)
  for a = b, e - 4, 4 do out[#out + 1] = ffi.cast("int32_t*", a)[0] end
  return out
end

-- The cooldown map (std::map<int, ...> at Creature+0x139C: head pointer, then size; a node holds left, parent, right, then
-- the colour and isnil bytes, then the pair at +0x10). Returns { [id] = "<4 bytes after the key as an int>/<as a float>" }
local function cooldown_entries(p)
  local head = rd32(p + P_COOLDOWNS)
  local out, count = {}, 0
  if not head then return out, 0 end
  local stack = { rd32(head + 4) }
  while #stack > 0 and count < 64 do
    local n = table.remove(stack)
    local node = n and kyubu.read(n, 0x18)
    if n and n ~= head and node and node:byte(14) == 0 then
      count = count + 1
      out[ffi.cast("int32_t*", node:sub(17, 20))[0]] = ("%d/%.1f"):format(ffi.cast("int32_t*", node:sub(21, 24))[0],
        ffi.cast("float*", node:sub(21, 24))[0])
      stack[#stack + 1] = ffi.cast("uint32_t*", node:sub(1, 4))[0]
      stack[#stack + 1] = ffi.cast("uint32_t*", node:sub(9, 12))[0]
    end
  end
  return out, rd32(p + P_COOLDOWNS + 4) or 0
end

-- What the hook watches each frame, logged only when it changes: the player's mode, the key-4 byte (GameController+9),
-- whether the game lets the id be used, and the id's cooldown-map entry
local seen = { mode = -1, key = -1, can = "", cd = "", bar = "" }
local function observe(gc, p)
  local mode = ffi.cast("uint8_t*", p + P_MODE)[0]
  local key = ffi.cast("uint8_t*", gc + 9)[0]
  local can = tostring(can_use(ffi.cast("void*", p), ID))
  local entries = cooldown_entries(p)
  local cd = entries[ID] or "none"
  if mode ~= seen.mode then
    kyubu.info(("slot4_proto: mode %d -> %d (mode time %d ms, mp %.3f)"):format(seen.mode, mode, tonumber(u32(p + P_TIME)),
      ffi.cast("float*", p + P_MP)[0]))
    seen.mode = mode
  end
  if key ~= seen.key then kyubu.info(("slot4_proto: key-4 byte %d -> %d (mode %d)"):format(seen.key, key, mode)); seen.key = key end
  if can ~= seen.can then kyubu.info(("slot4_proto: canUseAbility(%d) = %s (mode %d)"):format(ID, can, mode)); seen.can = can end
  local bar = ("class %d spec %d bar [%s]"):format(ffi.cast("uint8_t*", p + P_CLASS)[0], ffi.cast("uint8_t*", p + P_SPEC)[0],
    table.concat(bar_of(gc), ", "))
  if bar ~= seen.bar then kyubu.info("slot4_proto: " .. bar); seen.bar = bar end
  if cd ~= seen.cd then kyubu.info(("slot4_proto: cooldown entry of %d: %s (mode %d)"):format(ID, cd, mode)); seen.cd = cd end
end

local hook_id, err = kyubu.hook_mid(0x48BFDC, function(ctx)
  local gc = u32(ctx.esp + 0x10)
  local vec = u32(ctx.esp + 0x34)
  if gc == 0 or vec ~= gc + GC_BAR or player_of(gc) == 0 then return end
  local size = (u32(vec + 4) - u32(vec)) / 4
  if size ~= last_size then
    kyubu.info(("slot4_proto: bar size %d at the site (was %d)"):format(size, last_size))
    last_size = size
  end
  if ID ~= 0 and size == 5 then
    local value = ffi.new("int[1]", ID)
    push_back(ffi.cast("void*", vec), value)
    pushed = pushed + 1
    if pushed == 1 then kyubu.info(("slot4_proto: pushed %d onto the bar (first time)"):format(ID)) end
  else
    skipped = skipped + 1
  end
  if LEARN then
    local skills = ffi.cast("uint32_t*", player_of(gc) + 0x1150)
    for i = 0, 2 do if skills[i] == 0 then skills[i] = 1 end end
  end
  local ok, e = pcall(observe, gc, player_of(gc))
  if not ok and not seen.failed then seen.failed = true; kyubu.info("slot4_proto: observe failed: " .. tostring(e)) end
end)
kyubu.info(("slot4_proto: mid hook at 0x48BFDC %s"):format(hook_id and "installed" or ("FAILED: " .. tostring(err))))

kyubu.command("slot4_proto:id", "slot4_proto:id <n>: the ability id pushed as slot 4 (0 stops)", function(args, print)
  local n = tonumber(args)
  if n then ID = math.floor(n) end
  local text = ("slot4_proto: id = %d, pushed %d, skipped %d"):format(ID, pushed, skipped)
  kyubu.info(text); print(text)
end)

kyubu.command("slot4_proto:bar", "slot4_proto:bar: the skill bar's ids as the game holds them", function(_, print)
  local gc = game_controller()
  if gc == 0 then print("slot4_proto: no game controller"); return end
  local ids = bar_of(gc)
  local text = ("slot4_proto: bar size %d: [%s]; pushed %d times"):format(#ids, table.concat(ids, ", "), pushed)
  kyubu.info(text); print(text)
end)

-- The player's state: class, specialization, mode, mode time, mp, and the game's own answers for ID
kyubu.command("slot4_proto:state", "slot4_proto:state: class, specialization, mode, mp and the game's answers for the id", function(_, print)
  local gc = game_controller()
  local p = player_of(gc)
  if p == 0 then print("slot4_proto: no player"); return end
  local b = function(off) return ffi.cast("uint8_t*", p + off)[0] end
  local text = ("slot4_proto: class %d spec %d mode %d time %d mp %.3f; id %d: can_use %s cost %.4f cooldown %d"):format(
    b(P_CLASS), b(P_SPEC), b(P_MODE), tonumber(u32(p + P_TIME)), ffi.cast("float*", p + P_MP)[0],
    ID, tostring(can_use(ffi.cast("void*", p), ID)), cost_of(ffi.cast("void*", p), ID, -1),
    cooldown_of(ffi.cast("void*", p), ID, -1))
  kyubu.info(text); print(text)
end)

kyubu.command("slot4_proto:cooldowns", "slot4_proto:cooldowns: every entry of the player's cooldown map", function(_, print)
  local p = player_of(game_controller())
  if p == 0 then print("slot4_proto: no player"); return end
  local entries, size = cooldown_entries(p)
  local out = {}
  for id, v in pairs(entries) do out[#out + 1] = ("%d=%s"):format(id, v) end
  table.sort(out)
  local text = ("slot4_proto: cooldown map size %d: %s"):format(size, table.concat(out, " "))
  kyubu.info(text); print(text)
end)

-- Task 2.4 (probe ability-stats): a function hook on ability_cooldown (0x43EF40: __thiscall, ability id and skill level on the
-- stack, `ret 8`, the milliseconds in EAX) that returns a marked value for one id and passes every other call through. The
-- skill tooltip (skill_description_fill) and World::update both call it. `slot4_proto:mark <id> <ms>` installs it, `mark 0`
-- removes it (the tracer cannot trace a function that is hooked here, so the probe removes the mark before tracing).
local mark = { id = 0, ms = 0, hits = 0, hook = nil }
local function mark_status()
  return ("slot4_proto: mark %s, hits %d"):format(mark.hook and ("id %d -> %d ms"):format(mark.id, mark.ms) or "off", mark.hits)
end
kyubu.command("slot4_proto:mark", "slot4_proto:mark <id> <ms>: ability_cooldown returns <ms> for <id> (0 removes the hook)", function(args, print)
  local id, ms = tostring(args or ""):match("(%-?%d+)%s+(%-?%d+)")
  if mark.hook then kyubu.unhook(mark.hook); mark.hook = nil end
  if id and tonumber(id) ~= 0 then
    mark.id, mark.ms = math.floor(tonumber(id)), math.floor(tonumber(ms))
    local hid, herr = kyubu.hook("ability_cooldown", "int(__thiscall*)(void*, int, int)", function(next, self, ability, level)
      if ability == mark.id then mark.hits = mark.hits + 1; return mark.ms end
      return next(self, ability, level)
    end)
    mark.hook = hid
    if not hid then kyubu.info("slot4_proto: ability_cooldown hook FAILED: " .. tostring(herr)) end
  end
  local text = mark_status()
  kyubu.info(text); print(text)
end)
kyubu.command("slot4_proto:markstatus", "slot4_proto:markstatus: whether the ability_cooldown mark is on and how often it answered", function(_, print)
  local text = mark_status()
  kyubu.info(text); print(text)
end)

-- Task 2.6 (probe ability-icons): the icon map at GameController+0x800818, a std::map<int, T*> (head pointer at +0, size at +4;
-- a node holds left, parent, right, the colour and isnil bytes, the key at +0x10 and the 4-byte value at +0x14). The game's
-- operator[] (0x4044D0) inserts a node with a null value when the HUD looks up an id that has none, and the HUD skips a null
-- value. `find_icon` walks the tree the way 0x4044D0 does but never inserts; `slot4_proto:icon <id> <source id>` copies the
-- source node's value into the id's node and `slot4_proto:icon <id> 0 reset` writes null back (what a clean exit needs).
local GC_ICONS = 0x800818
local function find_icon(gc, id)
  local map = gc + GC_ICONS
  local head = rd32(map)
  if not head then return nil end
  local node, cand = rd32(head + 4), head
  local steps = 0
  while node and steps < 256 do
    local flag = kyubu.read(node + 0xD, 1)
    if not flag or flag:byte(1) ~= 0 then break end
    local key = rd32(node + 0x10)
    if not key then return nil end
    if ffi.cast("int32_t*", ffi.new("uint32_t[1]", key))[0] < id then node = rd32(node + 8)
    else cand = node; node = rd32(node) end
    steps = steps + 1
  end
  if cand == head then return nil end
  local ckey = rd32(cand + 0x10)
  if not ckey or id < ffi.cast("int32_t*", ffi.new("uint32_t[1]", ckey))[0] then return nil end
  return cand + 0x14
end
local function icon_text(gc, id)
  local at = find_icon(gc, id)
  if not at then return ("%d: no node"):format(id) end
  return ("%d: node value at 0x%X = 0x%X"):format(id, at, rd32(at) or 0)
end
kyubu.command("slot4_proto:icons", "slot4_proto:icons [ids]: the icon map's size and the value of each id's node (default 0 54 57 115)", function(args, print)
  local gc = game_controller()
  if gc == 0 then print("slot4_proto: no game controller"); return end
  local ids = {}
  for n in tostring(args or ""):gmatch("%-?%d+") do ids[#ids + 1] = tonumber(n) end
  if #ids == 0 then ids = { 0, 54, 57, 115 } end
  local parts = {}
  for _, id in ipairs(ids) do parts[#parts + 1] = icon_text(gc, id) end
  local text = ("slot4_proto: icon map size %d; %s"):format(rd32(gc + GC_ICONS + 4) or -1, table.concat(parts, "; "))
  kyubu.info(text); print(text)
end)
kyubu.command("slot4_proto:icon", "slot4_proto:icon <id> <source id> | <id> 0 reset: copy the source node's icon into the id's node (reset writes null)", function(args, print)
  local gc = game_controller()
  if gc == 0 then print("slot4_proto: no game controller"); return end
  local id, src, reset = tostring(args or ""):match("(%-?%d+)%s+(%-?%d+)%s*(%a*)")
  id, src = tonumber(id), tonumber(src)
  if not id then print("slot4_proto: usage: icon <id> <source id> | icon <id> 0 reset"); return end
  local at = find_icon(gc, id)
  if not at then local t = ("slot4_proto: icon %d: no node (the HUD has not looked it up yet)"):format(id); kyubu.info(t); print(t); return end
  local value = 0
  if reset ~= "reset" then
    local sat = find_icon(gc, src)
    value = sat and rd32(sat) or 0
    if value == 0 then local t = ("slot4_proto: icon %d: source %d has no node or a null value, nothing written"):format(id, src); kyubu.info(t); print(t); return end
  end
  local before = rd32(at) or 0
  kyubu.write(at, ffi.string(ffi.new("uint32_t[1]", value), 4))
  local t = ("slot4_proto: icon %d: value 0x%X -> 0x%X (source %s)"):format(id, before, rd32(at) or 0, reset == "reset" and "reset" or tostring(src))
  kyubu.info(t); print(t)
end)
