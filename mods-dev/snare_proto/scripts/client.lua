-- snare_proto, client side (task 9.2b of add-skills-unleashed, probe snare-freeze). The Scout's snare as the user wants it (2026-10-18):
-- the target is frozen in place, its animation stopped, no stun look and no stun mark; the only sign is that it turns whitish.
--   stand   every World step the creature's horizontal velocity and knockback are zeroed (lib/combat.stand_still)
--   nobrain its behaviour pointer (Creature+0x13E4) is taken off and put back at the end: no AI, so no walking, no turning and no attack
--           (the user's play-test 2026-10-18: zeroing the velocity left the creature moving at 1%, turning and hitting; an emptied threat
--           map did not stop the attacks either; the stun stops everything but lies the creature down and shows stars: rejected)
--   freeze  at the draw join site (draw_creature_join_site 0x41F135, EDI the draw record at Creature+0x1498) the record's words
--           +0x10 to +0x4EC are saved on the first draw after the snare and written back at every draw after it
--   white   the draw colour (vec4 at [ESP+0x14] at creature_draw_site) is multiplied by white_level (3): overbright, whitish; done as the Guardian's taunt tint is
--   stun, hidemark   the older way (stun timer held, the stun mark's gate), off by default
-- The newest creature is put 8 blocks north and 7 east and walks to the player; 1.2 s later phase A (8 s): stand, calm, freeze, white;
-- phase B (8 s): freeze and white off, stand and calm on (what the creature does when only held); phase C: all off.
-- Console: snare_proto:all [radius] [seconds] snares every creature near the player (6 s, 10 s immune after); snare_proto:run, snare_proto:opt <name> <value>. Offsets are real offsets of cube::Creature (+0x10).
local ffi = require("ffi")

local C_ID, C_POS, C_VEL, C_EXTRA, C_MODE, C_STUN, C_HP = 0x08, 0x10, 0x34, 0x4C, 0x68, 0x12C, 0x16C
local REC = 0x1498
local REC_FIRST, REC_LAST = 0x10, 0x4F0          -- the record's bytes that are saved: after the creature pointer, before the part matrices
local W_PLAYER = 0xB8
local N_LEFT, N_PARENT, N_RIGHT, N_ISNIL, N_VALUE = 0x00, 0x04, 0x08, 0x0D, 0x18
local FRAME_DT = 8
local BLOCK = 65536
local SITE_GATE, SITE_MODEL = 0x4B5ED8, 0x4B5EF9
local model_hits, model_other = 0, 0

local opt = { stand = 0, stun = 0, hidemark = 0, freeze = 0, calm = 0, nobrain = 0, white = 0, white_level = 3.0, hookwhite = 1, flash = 0, poke = 1, start_north = 8, start_east = 7, snare_after = 1200 }
local C_MPF, C_THREAT, C_BRAIN = 0x1190, 0x13A4, 0x13E4
local brain_saved = nil
local hold = nil                 -- { x, y } where the snared creature is held (world units)
local poked = false
local hit_fixes = 0
local tint_hits = { a = 0, b = 0, c = 0 }
local SITE_DRAW_A, SITE_DRAW_B, SITE_DRAW_C = 0x4B5D8C, 0x4BA8D8, 0x4BA97F
local gate_hits = 0
local prev_rec, draws, dumped = nil, 0, {}
local REC_END = 0x4F0
local state, state_t = "idle", 0
local carrier_id = nil
local snap = nil                  -- the saved words of the record, once taken
local diff_last, diff_max, restored = 0, 0, 0
local since_log = 0
local counts = { steps = 0, join = 0, gate = 0 }
local pending_run = false

local function u8(a) return ffi.cast("uint8_t*", a)[0] end
local function u32(a) return ffi.cast("uint32_t*", a)[0] end
local function i32(a) return ffi.cast("int32_t*", a)[0] end
local function f32(a) return ffi.cast("float*", a)[0] end
local function i64(a) return tonumber(ffi.cast("int64_t*", a)[0]) end
local function put_f32(a, v) ffi.cast("float*", a)[0] = v end
local function put_i32(a, v) ffi.cast("int32_t*", a)[0] = v end

local function each_creature(world, fn)
  local head = u32(world + 4)
  if head == 0 then return end
  local node, n = u32(head + N_LEFT), 0
  while node ~= head and n < 20000 do
    local c = u32(node + N_VALUE)
    if c ~= 0 then fn(c) end
    local right = u32(node + N_RIGHT)
    if u8(right + N_ISNIL) == 0 then
      node = right
      while true do
        local l = u32(node + N_LEFT)
        if u8(l + N_ISNIL) ~= 0 then break end
        node = l
      end
    else
      local p = u32(node + N_PARENT)
      while u8(p + N_ISNIL) == 0 and node == u32(p + N_RIGHT) do node = p; p = u32(node + N_PARENT) end
      node = p
    end
    n = n + 1
  end
end

local function find_newest(world, player)
  local best, best_id = 0, nil
  each_creature(world, function(c)
    local id = i64(c + C_ID)
    if c ~= player and (best_id == nil or id < best_id) then best, best_id = c, id end
  end)
  return best, best_id
end

local function by_id(world, id)
  local found = 0
  each_creature(world, function(c) if i64(c + C_ID) == id then found = c end end)
  return found
end

local function dist_blocks(a, b)
  local pa, pb = ffi.cast("int64_t*", a + C_POS), ffi.cast("int64_t*", b + C_POS)
  local dx, dy = (tonumber(pa[0]) - tonumber(pb[0])) / BLOCK, (tonumber(pa[1]) - tonumber(pb[1])) / BLOCK
  return math.sqrt(dx * dx + dy * dy)
end

local function set_state(name)
  state, state_t = name, 0
  kyubu.info(("snare_proto: phase %s (stand %d nobrain %d stun %d freeze %d white %d)"):format(name, opt.stand, opt.nobrain, opt.stun, opt.freeze, opt.white))
end

-- empties a creature's threat map as the AI's own give-up does (the nodes are leaked: this is a probe): no target, so no attack
local function clear_threat(c)
  local head = u32(c + C_THREAT)
  if head == 0 then return end
  ffi.cast("uint32_t*", head + 0)[0] = head
  ffi.cast("uint32_t*", head + 4)[0] = head
  ffi.cast("uint32_t*", head + 8)[0] = head
  ffi.cast("uint32_t*", c + C_THREAT + 4)[0] = 0
end

-- ---- snare_proto:all: every creature near the player is snared (brief: 6 s, then 10 s immune) ---------------------------------
-- Per creature { ends, hold = { x, y }, brain, snap }: position held, no behaviour (no walking, turning or attack), stun cleared, hits do
-- not stun or push it, its pose is the one saved at its first draw, and it is tinted. Own clock (ms of the World steps).
local snared, immune_until = {}, {}
local pending_all = nil              -- { radius (blocks), ms }
local clock = 0
local ALL_IMMUNE_MS = 10000

local function release(c, s)
  if s.brain ~= 0 then ffi.cast("uint32_t*", c + C_BRAIN)[0] = s.brain end
end

local function all_step(world, player, dt)
  clock = clock + dt
  local req = pending_all
  pending_all = nil
  local seen, started, ended = {}, 0, 0
  each_creature(world, function(c)
    if c == player then return end
    local id = i64(c + C_ID)
    local s = snared[id]
    if not s and req and (immune_until[id] or 0) <= clock and dist_blocks(c, player) <= req.radius then
      local pos = ffi.cast("int64_t*", c + C_POS)
      s = { ends = clock + req.ms, hold = { tonumber(pos[0]), tonumber(pos[1]) }, brain = u32(c + C_BRAIN) }
      snared[id] = s
      started = started + 1
    end
    if not s then return end
    seen[id] = true
    if clock >= s.ends then
      release(c, s)
      snared[id] = nil
      immune_until[id] = clock + ALL_IMMUNE_MS
      ended = ended + 1
      return
    end
    local pos = ffi.cast("int64_t*", c + C_POS)
    pos[0], pos[1] = s.hold[1], s.hold[2]
    put_f32(c + C_VEL, 0); put_f32(c + C_VEL + 4, 0)
    put_f32(c + C_EXTRA, 0); put_f32(c + C_EXTRA + 4, 0); put_f32(c + C_EXTRA + 8, 0)   -- extra_vel, the knockback (x, y, z)
    if i32(c + C_STUN) > -3000 then put_i32(c + C_STUN, -4000) end
    ffi.cast("uint32_t*", c + C_BRAIN)[0] = 0
    put_i32(c + C_MODE, 0)
  end)
  for id in pairs(snared) do if not seen[id] then snared[id] = nil end end   -- a creature that left the world
  if started > 0 or ended > 0 then kyubu.info(("snare_proto: all: %d snared, %d released (%d held now)"):format(started, ended, (function() local n = 0 for _ in pairs(snared) do n = n + 1 end return n end)())) end
end

local function on_step(ctx)
  local world, player = ctx.edi, ctx.eax
  if world == 0 or player == 0 or player ~= u32(world + W_PLAYER) then return end
  local dt = i32(ctx.ebp + FRAME_DT)
  counts.steps = counts.steps + 1
  all_step(world, player, dt)
  state_t, since_log = state_t + dt, since_log + dt
  if pending_run then
    pending_run = false
    local c, id = find_newest(world, player)
    if c == 0 then kyubu.warn("snare_proto: no creature but the player: spawn a ZOMBIE first"); return end
    carrier_id = id
    local pos, ppos = ffi.cast("int64_t*", c + C_POS), ffi.cast("int64_t*", player + C_POS)
    pos[0], pos[1], pos[2] = ppos[0] + opt.start_east * BLOCK, ppos[1] + opt.start_north * BLOCK, ppos[2]
    set_state("approach")
  end
  if state == "idle" or not carrier_id then return end
  local c = by_id(world, carrier_id)
  if c == 0 then kyubu.info("snare_proto: the creature is gone"); state = "idle"; return end
  local d = dist_blocks(c, player)
  if state == "approach" and state_t >= opt.snare_after then
    opt.stand, opt.nobrain, opt.freeze, opt.white = 1, 1, 1, 1
    snap, diff_last, diff_max, restored = nil, 0, 0, 0
    set_state("A")
  elseif state == "A" and state_t >= 8000 then
    opt.freeze, opt.white = 0, 0
    set_state("B")
  elseif state == "B" and state_t >= 8000 then
    opt.stand, opt.nobrain = 0, 0
    set_state("C")
  elseif state == "C" and state_t >= 8000 then
    kyubu.info(("snare_proto: counters steps %d join %d gate %d"):format(counts.steps, counts.join, counts.gate))
    state = "idle"
    return
  end
  if opt.stand ~= 0 then
    local pos = ffi.cast("int64_t*", c + C_POS)
    if not hold then hold = { tonumber(pos[0]), tonumber(pos[1]) } end
    if state == "A" and opt.poke ~= 0 and not poked and state_t >= 3000 then
      -- a push and a stun as another ability or a bump would give them: they must change nothing
      poked = true
      put_f32(c + C_EXTRA, 8.0); put_f32(c + C_VEL, 6.0); put_i32(c + C_STUN, 2000)
      kyubu.info("snare_proto: poked the creature: knockback 8, velocity 6 and a stun of 2000")
    end
    pos[0], pos[1] = hold[1], hold[2]
    put_f32(c + C_VEL, 0); put_f32(c + C_VEL + 4, 0)
    put_f32(c + C_EXTRA, 0); put_f32(c + C_EXTRA + 4, 0); put_f32(c + C_EXTRA + 8, 0)   -- extra_vel, the knockback (x, y, z)
    -- a snared creature cannot be stunned: a stun timer above the mark's floor (-3000) is put below it, so no stun and no stars
    if i32(c + C_STUN) > -3000 then put_i32(c + C_STUN, -4000) end
  else
    hold, poked = nil, false
  end
  if opt.stun ~= 0 and i32(c + C_STUN) < 1000 then put_i32(c + C_STUN, 1000) end
  if opt.calm ~= 0 then clear_threat(c) end
  if opt.white ~= 0 and opt.flash > 0 then put_f32(c + 0x1184, opt.flash) end   -- the game's own hit flash, held
  if opt.nobrain ~= 0 then
    -- the AI is the creature's behaviour (Creature+0x13E4, zero for a creature with none, which stands still): taken off for the snare
    if brain_saved == nil then
      brain_saved = u32(c + C_BRAIN)
      kyubu.info(("snare_proto: behaviour 0x%X taken off (mode %d)"):format(brain_saved, i32(c + C_MODE)))
    end
    ffi.cast("uint32_t*", c + C_BRAIN)[0] = 0
    put_i32(c + C_MODE, 0)
  elseif brain_saved ~= nil then
    ffi.cast("uint32_t*", c + C_BRAIN)[0] = brain_saved
    kyubu.info(("snare_proto: behaviour 0x%X put back"):format(brain_saved))
    brain_saved = nil
  end
  if since_log >= 500 then
    since_log = 0
    kyubu.info(("snare_proto: t=%d %s dist %.1f vel (%.2f, %.2f) mode %d stun %d record pose %d mode time %d diff %d/%d restored %d gate %d fixes %d disp %.2f tint %d/%d/%d model %d/%d mpf %.3f yaw %.1f player hp %.0f")
      :format(state_t, state, d, f32(c + C_VEL), f32(c + C_VEL + 4), i32(c + C_MODE), i32(c + C_STUN), i32(c + REC + 8), i32(c + REC + 4),
        diff_last, diff_max, restored, gate_hits, hit_fixes, hold and math.sqrt(((tonumber(ffi.cast('int64_t*', c + C_POS)[0]) - hold[1]) ^ 2 + (tonumber(ffi.cast('int64_t*', c + C_POS)[1]) - hold[2]) ^ 2)) / BLOCK or 0, tint_hits.a, tint_hits.b, tint_hits.c, model_hits, model_other, f32(c + C_MPF), f32(c + 0x30), f32(player + C_HP)))
  end
end

-- the join site: EDI is the draw record of the creature being drawn
local function on_join(ctx)
  counts.join = counts.join + 1
  local rec = ctx.edi
  if rec == 0 then return end
  local creature = u32(rec)
  if creature == 0 then return end
  local sn = snared[i64(creature + C_ID)]
  if sn then
    local w = ffi.cast("uint32_t*", rec)
    local a, b = REC_FIRST / 4, REC_LAST / 4 - 1
    if not sn.snap then
      sn.snap = {}
      for i = a, b do sn.snap[i] = w[i] end
    else
      for i = a, b do w[i] = sn.snap[i] end
    end
    return
  end
  if opt.freeze == 0 or not carrier_id then return end
  if i64(creature + C_ID) ~= carrier_id then return end
  local words = ffi.cast("uint32_t*", rec)
  local first, last = REC_FIRST / 4, REC_LAST / 4 - 1
  if not snap then
    snap = {}
    prev_rec, draws = nil, 0
    for i = first, last do snap[i] = words[i] end
    kyubu.info("snare_proto: the record was saved at the first draw of the snare")
    return
  end
  draws = draws + 1
  local changed = {}
  local cur = {}
  for i = REC_FIRST / 4, REC_END / 4 - 1 do
    cur[i] = words[i]
    if prev_rec and prev_rec[i] ~= words[i] then changed[#changed + 1] = i end
  end
  if prev_rec and (draws == 3 or draws == 12 or draws == 30 or draws == 45) then
    local list = {}
    for _, i in ipairs(changed) do list[#list + 1] = ("%X"):format(i * 4) end
    kyubu.info(("snare_proto: draw %d: %d record words differ from the last draw before the restore, at +0x%s"):format(draws, #changed, table.concat(list, ",+0x")))
  end
  prev_rec = cur
  local diff = 0
  for i = first, last do
    if words[i] ~= snap[i] then diff = diff + 1; words[i] = snap[i] end
  end
  diff_last = diff
  if diff > diff_max then diff_max = diff end
  restored = restored + 1
end

-- the stun mark's gate: ESI the creature; the mark is drawn while its stun timer is above -3000
local function on_gate(ctx)
  counts.gate = counts.gate + 1
  local c = ctx.esi
  if c == 0 then return end
  if snared[i64(c + C_ID)] then put_i32(c + C_STUN, -4000); return end
  if opt.hidemark == 0 or not carrier_id or i64(c + C_ID) ~= carrier_id then return end
  put_i32(c + C_STUN, -4000)
  gate_hits = gate_hits + 1
end

local function install(name, site, fn)
  local id, err = kyubu.hook_mid(site, function(ctx)
    local ok, e = pcall(fn, ctx)
    if not ok then kyubu.warn(("snare_proto: %s failed: %s"):format(name, tostring(e))) end
  end)
  local where = type(site) == "number" and ("0x%X"):format(site) or tostring(site)
  kyubu.info(("snare_proto: hook %s at %s %s"):format(name, where, id and "installed" or ("FAILED: " .. tostring(err))))
end

install("step", "cube::World::abilityDispatchSite", on_step)
install("join", "draw_creature_join_site", on_join)
install("gate", "stun_mark_gate_site", on_gate)
-- a hit on the snared creature does not stun, slow or push it (cube::Hit: stun_duration +0x18, slow_duration +0x1C, hit_dir +0x38)
local function fix_hit(hit)
  local victim = i64(hit + 0x08)
  if not snared[victim] and (opt.stand == 0 or not carrier_id or victim ~= carrier_id) then return end
  put_i32(hit + 0x18, 0); put_i32(hit + 0x1C, 0)
  put_f32(hit + 0x38, 0); put_f32(hit + 0x3C, 0); put_f32(hit + 0x40, 0)
  hit_fixes = hit_fixes + 1
end
install("hit_list", "cube::World::hitListSite", function(ctx) fix_hit(ctx.esp + 0xC4) end)
install("hit_damage", "cube::World::hitDamageSite", function(ctx) if ctx.ebx ~= 0 then fix_hit(ctx.ebx) end end)
-- the colour of the creature's draw (vec4 at [ESP+0x14]): scaled up, it is overbright and shows whitish
local function tint(creature, ctx, which)
  if creature == 0 or opt.hookwhite == 0 then return end
  local cid = i64(creature + C_ID)
  if not snared[cid] and (opt.white == 0 or not carrier_id or cid ~= carrier_id) then return end
  tint_hits[which] = tint_hits[which] + 1
  local col = ffi.cast("float*", u32(ctx.esp + 0x14))
  col[0], col[1], col[2] = col[0] * opt.white_level, col[1] * opt.white_level, col[2] * opt.white_level   -- a multiply at creature_draw_site, as the Guardian's taunt tint (lib/sites.lua on_tint)
end
install("draw_a", "creature_draw_site", function(ctx) tint(ctx.esi, ctx, "a") end)
install("model", "stun_mark_model_site", function(ctx)
  -- reached only when the stun's mark is being drawn (the model of the mark is fetched here)
  local c = ctx.esi
  if c ~= 0 and carrier_id and i64(c + C_ID) == carrier_id then model_hits = model_hits + 1 else model_other = model_other + 1 end
end)

kyubu.command("snare_proto:run", "snare_proto:run: send the newest creature at the player and snare it 1.2 s later (about 30 s)", function(_, print)
  pending_run = true
  print("run queued")
end)

kyubu.command("snare_proto:all", "snare_proto:all [radius blocks, 15] [seconds, 6]: snare every creature near the player", function(args, print)
  local r, sec = args:match("^%s*(%S*)%s*(%S*)")
  pending_all = { radius = tonumber(r) or 15, ms = (tonumber(sec) or 6) * 1000 }
  print(("snaring everything within %g blocks for %g s (then %d s immune)"):format(pending_all.radius, pending_all.ms / 1000, ALL_IMMUNE_MS / 1000))
end)

kyubu.command("snare_proto:opt", "snare_proto:opt <name> <value>", function(args, print)
  local name, value = args:match("^(%S+)%s+(%S+)")
  local v = tonumber(value)
  if not name or opt[name] == nil or not v then return print("usage: snare_proto:opt <name> <number>; names: stand calm nobrain stun hidemark freeze white white_level hookwhite flash poke start_north start_east snare_after") end
  opt[name] = v
  print(name .. " = " .. tostring(v))
end)

kyubu.info("snare_proto: loaded")
