-- draw_record_proto, client side (task 2.1 of add-ability-animation, probe anim-draw-record). draw_creature (0x4129C0) is a
-- __thiscall with ten stack arguments (`ret 0x28`); `this` is a draw record that lives inside the creature, at
-- Creature + 0x1498: +0 the creature, +4 the mode time, +8 the mode id. The hooks only read. At the entry
-- it reads the record and the creature's own mode (Creature+0x68, a byte) and mode time (Creature+0x6C); after it, the record's
-- mode id again (the draw sets it to 0 when a timed mode ends). Lines are logged once per distinct situation (caller, who,
-- record mode, creature mode), with the ten arguments of the first such call; a tally follows every 200 draws.
local ffi = require("ffi")

local GC_PLAYER = 0x8006C8
local P_MODE, P_TIME = 0x68, 0x6C
local REC = 0x1498

local function u32(addr) return ffi.cast("uint32_t*", addr)[0] end
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

local tally, order, seen, draws, logged = {}, {}, {}, 0, 0
local MAX_LINES = 300

local function note(rec, caller, a)
  local creature = rd32(rec)
  if not creature or creature == 0 then
    -- a record with no creature (task 2.2: a preview widget's record may be its own): log each caller once
    local nkey = ("%08X/norecord"):format(caller)
    if not seen[nkey] then
      seen[nkey] = true
      kyubu.info(("draw_record: NEW %s: rec=%08X word0=%s; args %s"):format(nkey, rec, tostring(creature), a))
    end
    return
  end
  local rtime, rmode = rd32(rec + 4), rd32(rec + 8)
  local cmode, ctime = rd8(creature + P_MODE), rd32(creature + P_TIME)
  if not (rtime and rmode and cmode and ctime) then return end
  local who = (creature == player()) and "player" or "other"
  local inside = (rec == creature + REC) and "in" or "OUT"
  local key = ("%08X/%s/rec%d/mode%d"):format(caller, who, rmode, cmode)
  local t = tally[key]
  if not t then
    t = { n = 0, eq = 0, caller = caller, who = who, rmode = rmode, cmode = cmode, inside = inside, after0 = 0 }
    tally[key] = t
    order[#order + 1] = key
    if logged < MAX_LINES then
      logged = logged + 1
      kyubu.info(("draw_record: NEW %s: rec=%08X creature=%08X (record %s the creature at +0x%X) record mode time %d, mode %d; "
        .. "creature mode %d, mode_start_time %d; args %s"):format(key, rec, creature, inside, rec - creature, rtime, rmode, cmode,
        tonumber(ctime), a))
    end
  end
  t.n = t.n + 1
  if rtime == ctime then t.eq = t.eq + 1 end
  return t, creature
end

local function flush()
  for _, key in ipairs(order) do
    local t = tally[key]
    if t.n > 0 then
      kyubu.info(("draw_record: tally %s: %d draw(s), record time == creature time in %d, the draw reset the record mode in %d"):format(
        key, t.n, t.eq, t.after0))
      t.n, t.eq, t.after0 = 0, 0, 0
    end
  end
end

-- Entry hook (a mid hook on the first instruction, which the tracer has hooked before): ECX is the record, [ESP] the caller,
-- [ESP+4 ...] the ten arguments. A second mid hook on the function's `ret 0x28` (0x423E16) reads the record's mode id after the
-- draw; if the engine refuses that site the log says so and the "reset" counts stay 0.
local pending
local function fbits(bits) return ffi.cast("float*", ffi.new("uint32_t[1]", bits))[0] end

local hook_id, herr = kyubu.hook_mid(0x4129C0, function(ctx)
  pending = nil
  local good, e = pcall(function()
    local sp = ctx.esp
    local a = {}
    for i = 1, 10 do a[i] = u32(sp + 4 * i) end
    local args = ("%X %X %X %X %X(%.4g) %X %X %X %X %X"):format(a[1], a[2], a[3], a[4], a[5], fbits(a[5]), a[6], a[7], a[8], a[9], a[10])
    local rec = ctx.ecx
    local t = note(rec, u32(sp), args)
    if t then pending = { t = t, rec = rec } end
    draws = draws + 1
    if draws % 200 == 0 then flush() end
  end)
  if not good and not seen.failed then seen.failed = true; kyubu.info("draw_record: entry hook failed: " .. tostring(e)) end
end)
kyubu.info(("draw_record: mid hook at 0x4129C0 %s"):format(hook_id and "installed" or ("FAILED: " .. tostring(herr))))

local ret_id, rerr = kyubu.hook_mid(0x423E16, function(ctx)
  local p = pending
  pending = nil
  if p and rd32(p.rec + 8) == 0 and p.t.rmode ~= 0 then p.t.after0 = p.t.after0 + 1 end
end)
kyubu.info(("draw_record: mid hook at 0x423E16 (ret 0x28) %s"):format(ret_id and "installed" or ("FAILED: " .. tostring(rerr))))

-- Task 2.2: `draw_record_proto:aura 1` writes 0.5 to the player's float at Creature + 0x1190 (the factor render tests against 0
-- before it draws a creature), `draw_record_proto:aura 0` restores 0. Above 0 render is read to defer the draw to its later pass.
local AURA = 0x1190
kyubu.command("draw_record_proto:aura", "draw_record_proto:aura <0|1>: set the player's float at Creature+0x1190 to 0.5 or 0", function(args, print)
  local p = player()
  if p == 0 then print("no player yet"); return end
  local on = tostring(args):find("1", 1, true) ~= nil
  local ok = kyubu.write(p + AURA, ffi.string(ffi.new("float[1]", on and 0.5 or 0), 4))
  kyubu.info(("draw_record: aura %s on the player %08X, write %s"):format(on and "on" or "off", p, tostring(ok)))
  print(("aura %s"):format(on and "on" or "off"))
end)
