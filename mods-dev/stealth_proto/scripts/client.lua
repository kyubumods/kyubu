-- stealth_proto, client side (tasks 11.2a and 11.2b of add-skills-unleashed, probe ninja-scan-skip). The Ninja's true stealth:
--   drop   each hostile creature gets threat -1 for the player (cube::Creature::topThreat starts its best at 0, so a value below 0
--          is never the target and a map holding only it gives no target).
--   cut    a mid hook at cube::CombatBehavior::targetScanSite (EBX the candidate, XMM2 its squared distance) writes a huge XMM2
--          when the candidate is the player, so the first-target scan never picks the player.
--   hold   the player's stealth (Creature+0x1190) is written 1.0 at the start of each of its World steps; the value read there
--          is what the step before left, so it shows the step's drain.
-- stealth_proto:run plays a timeline: A control, B drop only, C drop + cut + hold, D all off. Every second it logs each hostile
-- creature (distance, top threat, its threat for the player), the player's stealth and the counters.
-- Offsets are real offsets of cube::Creature (the data starts at +0x10), as in skills_unleashed.lib.combat.
local ffi = require("ffi")

local C_ID, C_POS, C_HOSTILE, C_HP = 0x08, 0x10, 0x60, 0x16C
local C_THREAT, C_STEALTH = 0x13A4, 0x1190
local W_PLAYER = 0xB8
local N_LEFT, N_PARENT, N_RIGHT, N_ISNIL, N_KEY, N_VALUE = 0x00, 0x04, 0x08, 0x0D, 0x10, 0x18
local FRAME_DT = 8
local BLOCK = 65536
local FAR = 1.0e30

local on = { cut = false, hold = false, keep = false }   -- keep: the drop written again every step
local timeline = nil              -- { t0, i, events = { { at = ms, note, drop, cut, hold } } }
local elapsed, since_report = 0, 0
local player_now, world_now = 0, 0
local hit_lines = 0               -- hit lines logged in this phase (30 at most)
local counts = { steps = 0, scan = 0, scan_player = 0, skipped = 0, hits_on_player = 0, draw_b_player = 0, draw_c_player = 0 }
local map_slot = nil
local key = ffi.new("int64_t[1]")

local function u8(a) return ffi.cast("uint8_t*", a)[0] end
local function u32(a) return ffi.cast("uint32_t*", a)[0] end
local function i32(a) return ffi.cast("int32_t*", a)[0] end
local function f32(a) return ffi.cast("float*", a)[0] end
local function i64(a) return tonumber(ffi.cast("int64_t*", a)[0]) end

local function each_node(map, fn)
  local head = u32(map)
  if head == 0 then return end
  local node, n = u32(head + N_LEFT), 0
  while node ~= head and n < 20000 do
    fn(node)
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

local function each_creature(world, fn)
  each_node(world + 4, function(node)
    local c = u32(node + N_VALUE)
    if c ~= 0 then fn(c) end
  end)
end

-- cube::Creature::topThreat in Lua: the best starts at 0, a value not below it replaces it, key 0 skipped
local function top_threat(c)
  local best, best_id = 0.0, 0
  each_node(c + C_THREAT, function(node)
    local id = i64(node + N_KEY)
    if id ~= 0 and best <= f32(node + N_VALUE) then best, best_id = f32(node + N_VALUE), id end
  end)
  return best_id, best
end

local function threat_of(c, id)
  local found = nil
  each_node(c + C_THREAT, function(node) if i64(node + N_KEY) == id then found = f32(node + N_VALUE) end end)
  return found
end

local function set_threat(c, id, value)
  if not map_slot then return false end
  key[0] = id
  local slot = map_slot(ffi.cast("void*", c + C_THREAT), nil, key)
  if slot == nil then return false end
  slot[0] = value
  return true
end

local function dist(a, b)
  local dx = (i64(a + C_POS) - i64(b + C_POS)) / BLOCK
  local dy = (i64(a + C_POS + 8) - i64(b + C_POS + 8)) / BLOCK
  local dz = (i64(a + C_POS + 16) - i64(b + C_POS + 16)) / BLOCK
  return math.sqrt(dx * dx + dy * dy + dz * dz)
end

local function is_enemy(c) return u8(c + C_HOSTILE) == 1 and f32(c + C_HP) > 0 end

local function phase()
  return (on.cut and "cut " or "") .. (on.hold and "hold" or "") .. ((on.cut or on.hold) and "" or "off")
end

local function report(world, player, stealth_before)
  local pid = i64(player + C_ID)
  each_creature(world, function(m)
    if m ~= player and is_enemy(m) and dist(m, player) <= 60 then
      local top_id, top_v = top_threat(m)
      local t = threat_of(m, pid)
      kyubu.info(("stealth_proto: t=%d [%s] id %d dist %.1f top_id %d top_value %.2f threat_for_player %s")
        :format(elapsed, phase(), i64(m + C_ID), dist(m, player), top_id, top_v, t and ("%.2f"):format(t) or "none"))
    end
  end)
  kyubu.info(("stealth_proto: t=%d [%s] player stealth_at_step_start %.4f counters steps %d scan %d scan_player %d skipped %d hits_on_player %d draw_b_player %d draw_c_player %d")
    :format(elapsed, phase(), stealth_before, counts.steps, counts.scan, counts.scan_player, counts.skipped, counts.hits_on_player,
      counts.draw_b_player, counts.draw_c_player))
end

local function drop(world, player)
  local pid, n = i64(player + C_ID), 0
  each_creature(world, function(m)
    if m ~= player and is_enemy(m) then
      if set_threat(m, pid, -1.0) then n = n + 1 end
    end
  end)
  kyubu.info(("stealth_proto: t=%d drop: threat -1 for the player on %d creature(s)"):format(elapsed, n))
end

local function on_step(ctx)
  local world, c = ctx.edi, ctx.eax
  if world == 0 or c == 0 or c ~= u32(world + W_PLAYER) then return end   -- once per World step: the local player's turn
  player_now, world_now = c, world
  local dt = i32(ctx.ebp + FRAME_DT)
  counts.steps = counts.steps + 1
  elapsed, since_report = elapsed + dt, since_report + dt
  local stealth_before = f32(c + C_STEALTH)
  if timeline then
    while timeline.i <= #timeline.events and elapsed - timeline.t0 >= timeline.events[timeline.i].at do
      local e = timeline.events[timeline.i]
      timeline.i = timeline.i + 1
      kyubu.info(("stealth_proto: timeline +%d s: %s"):format(e.at / 1000, e.note))
      on.cut, on.hold, on.keep = e.cut or false, e.hold or false, e.keep or false
      hit_lines = 0
      if e.drop then drop(world, c) end
    end
    if timeline.i > #timeline.events then timeline = nil end
  end
  if on.hold then ffi.cast("float*", c + C_STEALTH)[0] = 1.0 end
  if on.keep then
    local pid = i64(c + C_ID)
    each_creature(world, function(m) if m ~= c and is_enemy(m) then set_threat(m, pid, -1.0) end end)
  end
  if since_report >= 1000 then
    since_report = 0
    report(world, c, stealth_before)
  end
end

local function on_scan(ctx)
  counts.scan = counts.scan + 1
  if ctx.ebx ~= 0 and ctx.ebx == player_now then
    counts.scan_player = counts.scan_player + 1
    if on.cut then
      ctx.xmm[2].f32[0] = FAR
      counts.skipped = counts.skipped + 1
    end
  end
end

-- the damage site: ESI is the victim creature, EBX the cube::Hit
local function on_hit(ctx)
  if ctx.esi ~= 0 and ctx.esi == player_now then
    counts.hits_on_player = counts.hits_on_player + 1
    if (on.cut or on.keep) and hit_lines < 30 and world_now ~= 0 then
      hit_lines = hit_lines + 1
      local aid, attacker = i64(ctx.ebx), nil
      each_creature(world_now, function(m) if i64(m + C_ID) == aid then attacker = m end end)
      local t = attacker and threat_of(attacker, i64(player_now + C_ID))
      local top_id = attacker and top_threat(attacker) or -1
      kyubu.info(("stealth_proto: t=%d [%s] hit on the player by %d: its threat for the player %s, its top id %d")
        :format(elapsed, phase(), aid, t and ("%.2f"):format(t) or "none", top_id))
    end
  end
end

kyubu.on("lifecycle.register", function()
  local ms = kyubu.symbol("creature_map_slot")
  if ms then map_slot = ffi.cast("float*(__fastcall*)(void*, void*, const int64_t*)", ms) end
  kyubu.info(("stealth_proto: creature_map_slot %s"):format(tostring(ms)))
end)

local function install(name, site, fn)
  local id, err = kyubu.hook_mid(site, function(ctx)
    local ok, e = pcall(fn, ctx)
    if not ok then kyubu.warn(("stealth_proto: %s failed: %s"):format(name, tostring(e))) end
  end)
  kyubu.info(("stealth_proto: hook %s at %s %s"):format(name, tostring(site), id and "installed" or ("FAILED: " .. tostring(err))))
end

install("step", "cube::World::abilityDispatchSite", on_step)
install("scan", "cube::CombatBehavior::targetScanSite", on_scan)
install("hit", "cube::World::hitDamageSite", on_hit)
install("draw_b", "creature_draw_site_b", function(ctx)
  if ctx.ecx == player_now then counts.draw_b_player = counts.draw_b_player + 1 end
end)
install("draw_c", "creature_draw_site_c", function(ctx)
  if ctx.ecx == player_now then counts.draw_c_player = counts.draw_c_player + 1 end
end)

-- One console step (a `console` input step takes 15 s under Wine) runs the whole check from inside the game.
kyubu.command("stealth_proto:run", "stealth_proto:run: control, drop, drop + cut + hold, all off (about 72 s)", function(_, print)
  timeline = { t0 = elapsed, i = 1, events = {
    { at = 0, note = "A control: the slimes fight the player" },
    { at = 12000, note = "B drop only: threat -1 for the player", drop = true },
    { at = 26000, note = "C drop + cut + hold", drop = true, cut = true, hold = true },
    { at = 40000, note = "C2 drop every step + cut + hold", drop = true, cut = true, hold = true, keep = true },
    { at = 56000, note = "D all off: cut, hold and drop released" },
    { at = 72000, note = "end" },
  } }
  print("stealth_proto: timeline started")
end)

kyubu.info("stealth_proto: loaded")
