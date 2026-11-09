-- pose_join_proto, client side (task 2.4 of add-ability-animation, probe pose-join). draw_creature (0x4129C0) is a __thiscall: ECX is the
-- draw record (Creature + 0x1498). 0x41F135 is the merge point after the pose switch and its smoothing, before the four adds of the hand
-- vectors (call 0x412920 on rec+0x30, +0x24, +0x18, +0xC); EDI holds the record there. The hook only reads. It logs the record's vectors
-- (offsets from the task 2.3 table) once per distinct (who, class, creature mode, pose id), at most MAX_PER_KEY lines for the player's
-- non-idle modes, and counts the join site against the entry of draw_creature per creature.
local ffi = require("ffi")

local JOIN, EARLY, ENTRY = 0x41F135, 0x41F04B, 0x4129C0
local GC_PLAYER, REC = 0x8006C8, 0x1498
local P_MODE, P_CLASS = 0x68, 0x140
local MAX_PER_KEY, MAX_LINES = 6, 400

local function rd32(addr)
  local bytes = kyubu.read(addr, 4)
  return bytes and ffi.cast("uint32_t*", bytes)[0] or nil
end
local function rd8(addr)
  local bytes = kyubu.read(addr, 1)
  return bytes and bytes:byte(1) or nil
end
local function player()
  local p = kyubu.symbol("cube::game_controller")
  local gc = p and rd32(p) or 0
  return (gc and gc ~= 0) and (rd32(gc + GC_PLAYER) or 0) or 0
end

local function vec(base, off)
  local f = ffi.cast("float*", base + off)
  return ("(%.2f %.2f %.2f)"):format(f[0], f[1], f[2])
end
local function flt(base, off) return ("%.2f"):format(ffi.cast("float*", base + off)[0]) end

-- model slots (task 2.3): 0xBC head, 0xC0 hair, 0xC4 body, 0xC8 shoulder2, 0xCC hands, 0xD0 wings, 0xD4 tail, 0xD8 feet, 0xDC/0xE0 weapons, 0xE4 shoulders, 0xE8 neck
local SLOTS = { {"head", 0xBC}, {"hair", 0xC0}, {"body", 0xC4}, {"sh2", 0xC8}, {"hands", 0xCC}, {"wings", 0xD0}, {"tail", 0xD4},
                {"feet", 0xD8}, {"wpnB", 0xDC}, {"wpnA", 0xE0}, {"shoulders", 0xE4}, {"neck", 0xE8} }
local function slots(rec)
  local out = {}
  for _, s in ipairs(SLOTS) do
    if ffi.cast("uint32_t*", rec + s[2])[0] ~= 0 then out[#out + 1] = s[1] end
  end
  return table.concat(out, ",")
end

local seen, count, logged = {}, {}, 0
local entries, joins, early, fires_by_creature, entry_by_creature = 0, 0, 0, {}, {}

local function flush()
  local n, mismatch = 0, 0
  for c, e in pairs(entry_by_creature) do
    n = n + 1
    if (fires_by_creature[c] or 0) ~= e then mismatch = mismatch + 1 end
  end
  kyubu.info(("pose_join: tally: %d draw_creature entries, %d hits at 0x%X, %d hits at 0x%X, %d creatures seen, %d with entries != hits at the join"):format(
    entries, joins, JOIN, early, EARLY, n, mismatch))
  entries, joins, early, fires_by_creature, entry_by_creature = 0, 0, 0, {}, {}
end

local id_e, err_e = kyubu.hook_mid(ENTRY, function(ctx)
  pcall(function()
    local c = rd32(ctx.ecx)
    if c and c ~= 0 then
      entries = entries + 1
      entry_by_creature[c] = (entry_by_creature[c] or 0) + 1
    end
  end)
end)
kyubu.info(("pose_join: entry hook at 0x%X %s"):format(ENTRY, id_e and "installed" or ("FAILED: " .. tostring(err_e))))

local failed = false
local id_j, err_j = kyubu.hook_mid(JOIN, function(ctx)
  local ok, e = pcall(function()
    local rec = ctx.edi
    local creature = ffi.cast("uint32_t*", rec)[0]
    if creature == 0 then return end
    joins = joins + 1
    fires_by_creature[creature] = (fires_by_creature[creature] or 0) + 1
    if joins % 300 == 0 then flush() end
    local pose = ffi.cast("uint32_t*", rec + 8)[0]
    local cmode, class = rd8(creature + P_MODE), rd8(creature + P_CLASS)
    local who = (creature == player()) and "player" or "other"
    local key = ("%s/class%s/mode%s/pose%d"):format(who, tostring(class), tostring(cmode), pose)
    count[key] = (count[key] or 0) + 1
    local limit = (who == "player") and ((cmode == 0) and 2 or MAX_PER_KEY) or 1
    if count[key] > limit or logged >= MAX_LINES then return end
    logged = logged + 1
    local extra = ""
    if who == "player" then
      -- the creature's own angle fields the draw reads (task 2.3 lead for key 4): +0xC0, +0xC4..+0xCC, +0xD0, +0xD4, +0xD8
      extra = (" creature+0xC0 %s +0xC4 %s +0xD0 %s +0xD4 %s +0xD8 %s"):format(flt(creature, 0xC0), vec(creature, 0xC4),
        flt(creature, 0xD0), flt(creature, 0xD4), flt(creature, 0xD8))
    end
    kyubu.info(("pose_join: %s #%d creature=%08X rec=%08X; rootEuler %s rootOff %s pivot %s body %s bodyEuler %s lean %s head %s headEuler %s "
      .. "handA %s handAEuler %s handB %s handBEuler %s tail %s twist %s; slots %s;%s"):format(key, count[key], creature, rec,
      vec(rec, 0x60), vec(rec, 0xAC), flt(rec, 0xB8), vec(rec, 0x6C), vec(rec, 0x54), vec(rec, 0x9C), vec(rec, 0x78), vec(rec, 0x84),
      vec(rec, 0x0C), vec(rec, 0x24), vec(rec, 0x18), vec(rec, 0x30), vec(rec, 0x90), flt(rec, 0xA8), slots(rec), extra))
  end)
  if not ok and not failed then failed = true; kyubu.info("pose_join: join hook failed: " .. tostring(e)) end
end)
kyubu.info(("pose_join: mid hook at the join site 0x%X %s"):format(JOIN, id_j and "installed" or ("FAILED (KY_E_UNSAFE or other): " .. tostring(err_j))))

-- the earlier candidate (0x41F04B, `mov eax,[edi]` plus `cmp byte [eax+0x68],0x1c`): counts only
local id_b, err_b = kyubu.hook_mid(EARLY, function(ctx) early = early + 1 end)
kyubu.info(("pose_join: counting hook at 0x%X %s"):format(EARLY, id_b and "installed" or ("FAILED: " .. tostring(err_b))))
