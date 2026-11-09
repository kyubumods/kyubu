-- taunt_proto, client side (task 7.2 of add-skills-unleashed). One mod for the four probes of the Guardian's taunt:
--   taunt-target        every World step the taunted creature's threat map (Creature+0x13A4) gets a large value for the taunter,
--                       so cube::Creature::topThreat() and the combat AI pick the taunter at any distance.
--   hit-damage-scale    a mid hook at cube::World::hitDamageSite (0x59483E): ESI the victim, EBX the cube::Hit; a hit of a taunted
--                       creature on its taunter gets its damage multiplied by `factor`. Every hit seen there is logged.
--   creature-tint       mid hooks before the three draw_creature calls of render: the colour vec4 (arg 6, [ESP+0x14]) of a
--                       taunted creature gets its green and blue multiplied.
--   overhead-indicator  the stun's mark (a model slot drawn over the head while Creature+0x12C is above -3000) is drawn on a
--                       taunted creature, with the model of slot `mark_slot` in its place.
-- Console commands (the work is done inside the next World step, where the creature map is safe to read):
--   taunt_proto:taunt [blocks]   hostile creatures within `blocks` (50) of the player are taunted by the player
--   taunt_proto:decoy [blocks]   as taunt, but the taunter is the hostile creature with the highest id: the player is not it
--   taunt_proto:release          ends every taunt
--   taunt_proto:run              the whole check on a timeline (place, control 15 s, taunt, scale/tint/mark off and on, release)
--   taunt_proto:opt <name> <value>   factor, tint_g, tint_b, mark_slot, big, tint (0/1), mark (0/1), scale (0/1), watch (0/1)
-- Offsets are real offsets of cube::Creature (the data starts at +0x10), as in skills_unleashed.lib.combat.
local ffi = require("ffi")
local bit = require("bit")

local C_ID, C_POS, C_HOSTILE, C_STUN, C_HP = 0x08, 0x10, 0x60, 0x12C, 0x16C
local C_THREAT, C_HITS, C_FLASH, C_MPF = 0x13A4, 0x13AC, 0x1184, 0x1190
local W_PLAYER = 0xB8
local N_LEFT, N_PARENT, N_RIGHT, N_ISNIL, N_KEY, N_VALUE = 0x00, 0x04, 0x08, 0x0D, 0x10, 0x18
local FRAME_DT = 8
local BLOCK = 65536
local SITE_HIT_DAMAGE = 0x59483E
local SITE_DRAW_A, SITE_DRAW_B, SITE_DRAW_C = 0x4B5D8C, 0x4BA8D8, 0x4BA97F
local SITE_MARK_GATE, SITE_MARK_MODEL = 0x4B5ED8, 0x4B5EF9
local STUN_FLOOR = -3000

local opt = { factor = 0.65, tint_g = 0.35, tint_b = 0.35, mark_slot = 0x9F8, big = 1.0e7, tint = 1, mark = 1, scale = 1, watch = 1, neg = 1, defer = 0 }
local taunts = {}            -- creature id -> { by = taunter id }
local pending = nil          -- a command waiting for the next World step: { what, blocks }
local timeline = nil         -- `run`: { t0, i, events = { { at = ms, note =, what =, blocks =, opt = { name, value } } } }
local world_now, player_now = 0, 0
local elapsed, since_report = 0, 0
local counts = { steps = 0, draw_a = 0, draw_b = 0, draw_c = 0, mark = 0, hits = 0, scaled = 0 }
local hit_lines, other_lines = 0, 0
local hits_by = {}                   -- attacker id -> hits seen at the damage site
local alert_fn, enemies_fn = nil, nil
local frame_now = 0

local function u8(a) return ffi.cast("uint8_t*", a)[0] end
local function u32(a) return ffi.cast("uint32_t*", a)[0] end
local function i32(a) return ffi.cast("int32_t*", a)[0] end
local function f32(a) return ffi.cast("float*", a)[0] end
local function i64(a) return tonumber(ffi.cast("int64_t*", a)[0]) end

-- the std::map at World+4 (key at +0x10, creature at +0x18); the same walk for any of the game's maps
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

-- cube::Creature::topThreat (0x4451F0) in Lua: the key of the largest value, a tie to the later key, 0 when none
local function top_threat(c)
  local best, best_id = 0.0, 0
  each_node(c + C_THREAT, function(node)
    local id = i64(node + N_KEY)
    if id ~= 0 and best <= f32(node + N_VALUE) then best, best_id = f32(node + N_VALUE), id end
  end)
  return best_id, best
end

-- the float a creature's std::map member at `offset` (threat C_THREAT, hit counts C_HITS) holds for `key`, nil when absent
local function map_value(c, offset, key)
  local found = nil
  each_node(c + offset, function(node)
    if i64(node + N_KEY) == key then found = f32(node + N_VALUE) end
  end)
  return found
end

local map_slot = nil
local key = ffi.new("int64_t[1]")
local function set_threat(c, id, value)
  key[0] = id
  local slot = map_slot(ffi.cast("void*", c + C_THREAT), nil, key)
  if slot ~= nil then slot[0] = value end
end

local get_model = nil

-- Empties a creature's threat map as the AI's own give-up does (the nodes are leaked: this is a probe): no target, no memory of
-- the player. Without it a creature that once fought the player keeps chasing it from any distance (the entry stays).
local function clear_threat(c)
  local head = u32(c + C_THREAT)
  if head == 0 then return end
  ffi.cast("uint32_t*", head + 0)[0] = head
  ffi.cast("uint32_t*", head + 4)[0] = head
  ffi.cast("uint32_t*", head + 8)[0] = head
  ffi.cast("uint32_t*", c + C_THREAT + 4)[0] = 0
end

local function pos_blocks(c)
  return i64(c + C_POS) / BLOCK, i64(c + C_POS + 8) / BLOCK, i64(c + C_POS + 16) / BLOCK
end

local function dist(a, b)
  local ax, ay, az = pos_blocks(a)
  local bx, by, bz = pos_blocks(b)
  return math.sqrt((ax - bx) ^ 2 + (ay - by) ^ 2 + (az - bz) ^ 2)
end

local function find_by_id(world, id)
  local found = 0
  each_creature(world, function(c) if i64(c + C_ID) == id then found = c end end)
  return found
end

local function is_enemy(c) return u8(c + C_HOSTILE) == 1 and f32(c + C_HP) > 0 end
-- the creatures the commands act on: hostile and alive; with `neg` on, only the spawner's (their ids are negative)
local function eligible(c) return is_enemy(c) and (opt.neg == 0 or i64(c + C_ID) < 0) end

local function start_taunt(world, player, what, blocks)
  taunts = {}
  local list = {}
  each_creature(world, function(c)
    if c ~= player and eligible(c) and dist(c, player) <= blocks then list[#list + 1] = c end
  end)
  local taunter_id, taunter = i64(player + C_ID), player
  if what == "decoy" then
    local top = 0
    for _, c in ipairs(list) do if i64(c + C_ID) > top then top, taunter = i64(c + C_ID), c end end
    taunter_id = top
  end
  for _, c in ipairs(list) do
    local id = i64(c + C_ID)
    if id ~= taunter_id then
      taunts[id] = { by = taunter_id }
      local top_id, top_v = top_threat(c)
      kyubu.info(("taunt_proto: taunt id %d at %.1f blocks from the player, %.1f from the taunter %d, top threat before: id %d value %.2f, hp %.1f")
        :format(id, dist(c, player), taunter ~= 0 and dist(c, taunter) or -1, taunter_id, top_id, top_v, f32(c + C_HP)))
    end
  end
  kyubu.info(("taunt_proto: %s of %d creature(s) by id %d (the player is id %d)"):format(what, #list, taunter_id, i64(player + C_ID)))
end

local function report(world, player)
  local pid = i64(player + C_ID)
  for id, t in pairs(taunts) do
    local c = find_by_id(world, id)
    if c ~= 0 then
      local taunter = t.by == pid and player or find_by_id(world, t.by)
      local top_id, top_v = top_threat(c)
      kyubu.info(("taunt_proto: t=%d id %d hp %.1f dist_to_player %.1f dist_to_taunter %.1f top_id %d top_value %.1f stun %d hit_count %s hits_seen %d player_flash %.3f")
        :format(elapsed, id, f32(c + C_HP), dist(c, player), taunter ~= 0 and dist(c, taunter) or -1, top_id, top_v, i32(c + C_STUN),
          tostring(map_value(c, C_HITS, pid)), hits_by[id] or 0, f32(player + C_FLASH)))
    else
      kyubu.info(("taunt_proto: t=%d id %d is gone"):format(elapsed, id))
    end
  end
  kyubu.info(("taunt_proto: counters steps %d draw_a %d draw_b %d draw_c %d mark %d hits %d scaled %d"):format(
    counts.steps, counts.draw_a, counts.draw_b, counts.draw_c, counts.mark, counts.hits, counts.scaled))
end

-- creatures_are_enemies (0x5954F0) on pairs, and cube::World::alertAllies (0x59F260): the player attacks the nearest hostile creature
-- (`victim`), every other one within 8 blocks of it that holds no threat for the player should get 0.5
local function run_check(world, player, frame)
  local list = {}
  each_creature(world, function(m) if m ~= player and eligible(m) then list[#list + 1] = m end end)
  table.sort(list, function(a, b) return dist(a, player) < dist(b, player) end)
  if #list < 3 or not enemies_fn or not alert_fn then
    kyubu.info(("taunt_proto: check skipped: %d creatures, enemies_fn %s, alert_fn %s"):format(#list, tostring(enemies_fn), tostring(alert_fn)))
    return
  end
  local s1, s2 = list[1], list[2]
  local function en(a, b) return enemies_fn(ffi.cast("void*", a), ffi.cast("void*", b)) ~= 0 end
  kyubu.info(("taunt_proto: check enemies: (slime, player) %s, (player, slime) %s, (slime, slime) %s")
    :format(tostring(en(s1, player)), tostring(en(player, s1)), tostring(en(s1, s2))))
  for _, m in ipairs(list) do clear_threat(m) end
  local pid = i64(player + C_ID)
  local step_out = u32(frame - 0x2B28)
  alert_fn(ffi.cast("void*", world), nil, ffi.cast("void*", player), ffi.cast("void*", s1), ffi.cast("void*", step_out))
  for i, m in ipairs(list) do
    kyubu.info(("taunt_proto: check alert: creature id %d is %.1f blocks from the victim (id %d): threat for the player %s")
      :format(i64(m + C_ID), dist(m, s1), i64(s1 + C_ID), tostring(map_value(m, C_THREAT, pid))))
  end
end

local function on_step(ctx)
  local world, c = ctx.edi, ctx.eax
  if world == 0 or c == 0 or c ~= u32(world + W_PLAYER) then return end   -- once per World step: the local player's turn
  world_now, player_now = world, c
  frame_now = ctx.ebp
  local dt = i32(ctx.ebp + FRAME_DT)
  counts.steps = counts.steps + 1
  elapsed, since_report = elapsed + dt, since_report + dt
  if timeline then
    while timeline.i <= #timeline.events and elapsed - timeline.t0 >= timeline.events[timeline.i].at do
      local e = timeline.events[timeline.i]
      timeline.i = timeline.i + 1
      kyubu.info(("taunt_proto: timeline +%d s: %s"):format(e.at / 1000, e.note))
      if e.opt then opt[e.opt[1]] = e.opt[2] end
      if e.what then pending = e end
    end
    if timeline.i > #timeline.events then timeline = nil end
  end
  if pending then
    local p = pending
    pending = nil
    if p.what == "release" then
      taunts = {}
      kyubu.info("taunt_proto: released")
    elseif p.what == "check" then
      run_check(world, c, frame_now)
    elseif p.what == "place" then
      -- puts the hostile creatures in a line north of the player, the highest id nearest, `blocks` and then 4 blocks further each,
      -- and empties their threat maps
      local list = {}
      each_creature(world, function(m) if m ~= c and eligible(m) then list[#list + 1] = m end end)
      table.sort(list, function(a, b) return i64(a + C_ID) > i64(b + C_ID) end)
      for i, m in ipairs(list) do
        ffi.cast("int64_t*", m + C_POS)[0] = ffi.cast("int64_t*", c + C_POS)[0] + (i - 3) * 2 * BLOCK
        ffi.cast("int64_t*", m + C_POS + 8)[0] = ffi.cast("int64_t*", c + C_POS + 8)[0] + (p.blocks + 4 * (i - 1)) * BLOCK
        ffi.cast("int64_t*", m + C_POS + 16)[0] = ffi.cast("int64_t*", c + C_POS + 16)[0] + BLOCK
        clear_threat(m)
      end
      kyubu.info(("taunt_proto: placed %d hostile creature(s) from %d blocks north"):format(#list, p.blocks))
    else
      start_taunt(world, c, p.what, p.blocks)
    end
  end
  if next(taunts) == nil then
    if opt.watch ~= 0 and since_report >= 1000 then      -- the control: hostile creatures near the player, no taunt
      since_report = 0
      each_creature(world, function(m)
        if m ~= c and eligible(m) and dist(m, c) <= 60 then
          local top_id, top_v = top_threat(m)
          kyubu.info(("taunt_proto: t=%d control id %d dist_to_player %.1f top_id %d top_value %.2f")
            :format(elapsed, i64(m + C_ID), dist(m, c), top_id, top_v))
        end
      end)
    end
    return
  end
  each_creature(world, function(m)
    local t = taunts[i64(m + C_ID)]
    if t and f32(m + C_HP) > 0 then
      set_threat(m, t.by, opt.big)
      if opt.defer ~= 0 then ffi.cast("float*", m + C_MPF)[0] = 0.5 end   -- drawn in render's later pass
    end
  end)
  if since_report >= 1000 then
    since_report = 0
    report(world, c)
  end
end

-- the damage site: ESI is the victim creature, EBX the cube::Hit (attacker id +0, victim id +8, damage +0x10)
local function on_hit(ctx)
  local hit = ctx.ebx
  if hit == 0 then return end
  local attacker, victim, damage = i64(hit), i64(hit + 8), f32(hit + 0x10)
  counts.hits = counts.hits + 1
  hits_by[attacker] = (hits_by[attacker] or 0) + 1
  local t = taunts[attacker]
  local scaled = damage
  if t and t.by == victim and opt.scale ~= 0 then
    scaled = damage * opt.factor
    ffi.cast("float*", hit + 0x10)[0] = scaled
    counts.scaled = counts.scaled + 1
  end
  local pid = player_now ~= 0 and i64(player_now + C_ID) or 0
  local interesting = t ~= nil or taunts[victim] ~= nil
  if (interesting and hit_lines < 400) or (not interesting and victim == pid and other_lines < 20) then
    if interesting then hit_lines = hit_lines + 1 else other_lines = other_lines + 1 end
    kyubu.info(("taunt_proto: hit attacker %d victim %d damage %.3f -> %.3f victim_hp_before %.2f taunted_attacker %s flash %.3f")
      :format(attacker, victim, damage, scaled, f32(ctx.esi + C_HP), tostring(t ~= nil), f32(ctx.esi + C_FLASH)))
  end
end

-- draw_creature's colour vec4 at [ESP+0x14]
local function tint(creature, ctx, counter)
  if opt.tint == 0 or creature == 0 then return end
  if not taunts[i64(creature + C_ID)] then return end
  local col = ffi.cast("float*", u32(ctx.esp + 0x14))
  col[1] = col[1] * opt.tint_g
  col[2] = col[2] * opt.tint_b
  counts[counter] = counts[counter] + 1
end

local faked, saved_stun = 0, 0

local function on_mark_gate(ctx)
  local c = ctx.esi
  if opt.mark == 0 or c == 0 or not taunts[i64(c + C_ID)] then return end
  local stun = i32(c + C_STUN)
  if stun > STUN_FLOOR then return end
  faked, saved_stun = c, stun
  ffi.cast("int32_t*", c + C_STUN)[0] = 1
end

local function on_mark_model(ctx)
  if faked == 0 then return end
  ffi.cast("int32_t*", faked + C_STUN)[0] = saved_stun
  faked = 0
  counts.mark = counts.mark + 1
  local models = u32(ctx.esp + 0x10C)
  if get_model and models ~= 0 and opt.mark_slot ~= 0x9F5 then
    local m = get_model(ffi.cast("void*", models), nil, opt.mark_slot)
    if m ~= nil then ctx.eax = tonumber(ffi.cast("uint32_t", m)) end
  end
end

kyubu.on("lifecycle.register", function()
  local ms = kyubu.symbol("creature_map_slot")
  if ms then map_slot = ffi.cast("float*(__fastcall*)(void*, void*, const int64_t*)", ms) end
  local gm = kyubu.symbol("ModelLoader::get")
  if gm then get_model = ffi.cast("void*(__fastcall*)(void*, void*, int32_t)", gm) end
  local al = kyubu.symbol("cube::World::alertAllies")
  if al then alert_fn = ffi.cast("void(__fastcall*)(void*, void*, void*, void*, void*)", al) end
  local en = kyubu.symbol("creatures_are_enemies")
  if en then enemies_fn = ffi.cast("uint8_t(__stdcall*)(void*, void*)", en) end
  kyubu.info(("taunt_proto: creature_map_slot %s, ModelLoader::get %s, alertAllies %s, creatures_are_enemies %s")
    :format(tostring(ms), tostring(gm), tostring(al), tostring(en)))
end)

local function install(name, site, fn)
  local id, err = kyubu.hook_mid(site, function(ctx)
    local ok, e = pcall(fn, ctx)
    if not ok then kyubu.warn(("taunt_proto: %s failed: %s"):format(name, tostring(e))) end
  end)
  local where = type(site) == "number" and ("0x%X"):format(site) or tostring(site)
  kyubu.info(("taunt_proto: hook %s at %s %s"):format(name, where, id and "installed" or ("FAILED: " .. tostring(err))))
end

install("step", "cube::World::abilityDispatchSite", on_step)
install("hit", SITE_HIT_DAMAGE, on_hit)
install("draw_a", SITE_DRAW_A, function(ctx) tint(ctx.esi, ctx, "draw_a") end)
install("draw_b", SITE_DRAW_B, function(ctx) tint(ctx.ecx, ctx, "draw_b") end)
install("draw_c", SITE_DRAW_C, function(ctx) tint(ctx.ecx, ctx, "draw_c") end)
install("mark_gate", SITE_MARK_GATE, on_mark_gate)
install("mark_model", SITE_MARK_MODEL, on_mark_model)

kyubu.command("taunt_proto:taunt", "taunt_proto:taunt [blocks]: the hostile creatures within 50 blocks target the player", function(args, print)
  pending = { what = "taunt", blocks = tonumber(args) or 50 }
  print("taunt queued for the next step")
end)

kyubu.command("taunt_proto:decoy", "taunt_proto:decoy [blocks]: as taunt, but the taunter is the creature with the highest id", function(args, print)
  pending = { what = "decoy", blocks = tonumber(args) or 50 }
  print("decoy taunt queued for the next step")
end)

kyubu.command("taunt_proto:place", "taunt_proto:place [blocks]: puts the hostile creatures in a line, the nearest [blocks] (26) north", function(args, print)
  pending = { what = "place", blocks = tonumber(args) or 26 }
  print("place queued for the next step")
end)

-- One console step (a `console` input step takes 15 s under Wine) runs the whole check from inside the game.
kyubu.command("taunt_proto:run", "taunt_proto:run [natural]: place, wait, taunt, then scale/tint/mark off and on again (about 95 s); natural: no place, every hostile creature counts", function(args, print)
  local natural = args:find("natural") ~= nil
  if natural then opt.neg = 0 end
  if args:find("visual") then
    -- for creatures that stand still in front of the player (spawner ... still): taunted at once, tint and mark switched in turn
    timeline = { t0 = elapsed, i = 1, events = {
      { at = 0,     note = "taunt by the player: tint and mark on", what = "taunt", blocks = 60 },
      { at = 20000, note = "tint off", opt = { "tint", 0 } },
      { at = 40000, note = "tint on, mark off", opt = { "tint", 1 } },
      { at = 40001, note = "mark off", opt = { "mark", 0 } },
      { at = 50000, note = "mark on, and the later draw pass (Creature+0x1190 above 0)", opt = { "mark", 1 } },
      { at = 50001, note = "defer on", opt = { "defer", 1 } },
      { at = 80000, note = "release", what = "release" },
    } }
    print("visual run queued")
    return
  end
  timeline = { t0 = elapsed, i = 1, events = {
    natural and { at = 0, note = "control: the creatures stay where they are" }
      or { at = 0, note = "place the slimes 26 blocks north and further", what = "place", blocks = 26 },
    { at = 5000,  note = "checks: creatures_are_enemies and alertAllies", what = "check" },
    { at = 15000, note = "taunt by the player", what = "taunt", blocks = 60 },
    { at = 45000, note = "damage scale off", opt = { "scale", 0 } },
    { at = 58000, note = "damage scale on", opt = { "scale", 1 } },
    { at = 62000, note = "tint off", opt = { "tint", 0 } },
    { at = 70000, note = "tint on", opt = { "tint", 1 } },
    { at = 74000, note = "mark off", opt = { "mark", 0 } },
    { at = 82000, note = "mark on", opt = { "mark", 1 } },
    { at = 95000, note = "release", what = "release" },
  } }
  hit_lines = 0
  print("run queued")
end)

kyubu.command("taunt_proto:release", "taunt_proto:release: ends every taunt", function(_, print)
  pending = { what = "release" }
  print("release queued for the next step")
end)

kyubu.command("taunt_proto:opt", "taunt_proto:opt <name> <value>: factor, tint_g, tint_b, mark_slot, big, tint, mark, scale, watch", function(args, print)
  local name, value = args:match("^(%S+)%s+(%S+)")
  local v = tonumber(value)
  if not name or opt[name] == nil or not v then return print("usage: taunt_proto:opt <name> <number>; names: factor tint_g tint_b mark_slot big tint mark scale watch") end
  opt[name] = v
  kyubu.info(("taunt_proto: opt %s = %s"):format(name, tostring(v)))
  print(name .. " = " .. tostring(v))
end)

kyubu.info("taunt_proto: loaded")
