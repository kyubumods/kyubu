-- behavior_proto, server side (the game thread, at frame begin: outside a World step): spawns slimes through
-- kyubu.game.creature_spawn (no behaviour: KY_SPAWN_STILL), then builds the zone's default behaviour for some of them
-- by calling the game's constructors and the list append through cubeworld's function types (task 6.1's prototype of
-- what KY_SPAWN_MONSTER will do). Cube.exe only: it reads the World through cube::game_controller.
local ffi = require("ffi")
local cw = require("cubeworld")

local UNITS_PER_BLOCK = 65536
local FRIENDLY, HOSTILE = 2, 1     -- cube::HostileType
local SLIME = 37                   -- cube::EntityType GREEN_SLIME

local function fn(name)
  local addr, why = kyubu.symbol(name)
  if not addr then error(("symbol %s: %s"):format(name, tostring(why)), 0) end
  return ffi.cast(cw.functions[name], addr)
end

-- The creature at `id` in the World's map (creature_map_slot finds an existing key), or nil.
local function creature_by_id(id)
  local gc_addr = assert(kyubu.symbol("cube::game_controller"))
  local gc = ffi.cast("uintptr_t*", gc_addr)[0]
  local map = ffi.cast("void*", gc + 0x2E8)           -- cube::World.creatures: World at +0x2DC, the map 4 in
  local key = ffi.new("uint64_t[1]", id)              -- a negative id as its two's complement
  local slot = fn("creature_map_slot")(map, nil, key)
  return slot[0] ~= nil and slot[0] or nil
end

-- Steps 8 of the recipe: the zone's default behaviour (a sequence of combat, look at the player and random walk).
local function give_default_behavior(creature)
  if creature.behavior ~= nil then return nil, "the creature has a behaviour already" end
  local new = fn("operator_new")
  local sequence = fn("cube::SequentialBehavior::SequentialBehavior")(new(12), nil)
  local children = {
    fn("cube::CombatBehavior::CombatBehavior")(new(20), nil, 20.0),
    fn("cube::LookAtPlayerBehavior::LookAtPlayerBehavior")(new(4), nil),
    fn("cube::RandomWalkBehavior::RandomWalkBehavior")(new(8), nil),
  }
  local list = ffi.cast("void*", ffi.cast("uintptr_t", sequence) + 4)
  local push_back = fn("std::list::push_back")
  local cell = ffi.new("void*[1]")
  for _, child in ipairs(children) do
    cell[0] = ffi.cast("void*", child)
    push_back(list, nil, cell)
  end
  creature.behavior = ffi.cast("cube_Behavior*", sequence)
  return sequence
end

-- The local player's hp and position, read from the creature the GameController keeps (creature_player is client-only).
local function player_state()
  local gc = ffi.cast("uintptr_t*", assert(kyubu.symbol("cube::game_controller")))[0]
  local player = ffi.cast("cube_Creature**", gc + 0x8006D0)[0]
  if player == nil then return nil end
  return { hp = player.data.hp, pos = { x = tonumber(player.data.pos.x), y = tonumber(player.data.pos.y) } }
end

local watched = {}   -- creature object -> { label =, last = seconds of the last line }

local function spawn(label, hostile, level, x, y, z, behavior)
  local creature, why = kyubu.game.creature_spawn({
    type = SLIME, hostile = hostile, level = level, pos = { x = x, y = y, z = z }, yaw = 0 })
  if not creature then return kyubu.warn(("%s: not spawned: %s"):format(label, tostring(why))) end
  local info = creature:get()
  local text = ("%s: id %d, hostile %d, hp %.1f at %.1f %.1f"):format(label, info.id, info.hostile, info.hp,
    info.pos.x / UNITS_PER_BLOCK, info.pos.y / UNITS_PER_BLOCK)
  if behavior then
    local raw = creature_by_id(info.id)
    if not raw then return kyubu.warn(label .. ": the creature is not in the map") end
    local ok, seq, err = pcall(give_default_behavior, raw)
    if not ok then return kyubu.warn(("%s: behaviour failed: %s"):format(label, tostring(seq))) end
    if not seq then return kyubu.warn(label .. ": " .. tostring(err)) end
    text = ("%s; behaviour %s (creature %s)"):format(text, tostring(seq), tostring(raw))
  end
  kyubu.info(text)
  watched[creature] = { label = label, id = info.id }
end

kyubu.channel.register("behavior_proto:spawn", function(_, m)
  if type(m) ~= "table" then return end
  local blocks = (tonumber(m.blocks) or 10) * UNITS_PER_BLOCK
  local level = math.max(1, tonumber(m.level) or 1)
  if m.kind == "all" then
    spawn("control (hostile, no behaviour)", HOSTILE, level, m.x - 6 * UNITS_PER_BLOCK, m.y + blocks, m.z, false)
    spawn("friendly (behaviour)", FRIENDLY, level, m.x + 6 * UNITS_PER_BLOCK, m.y + blocks, m.z, true)
  end
  spawn("monster (hostile, behaviour)", HOSTILE, level, m.x, m.y + blocks, m.z, true)
end)

-- Every two seconds: where each creature is (blocks from the player), the player's hp, and the creatures the game has
-- removed.
local ticks = 0
kyubu.on("sim.tick", function()
  ticks = ticks + 1
  if ticks % 20 ~= 0 then return end
  local pinfo = player_state()
  if not pinfo then return end
  local line = { ("player hp %.1f"):format(pinfo.hp) }
  for creature, w in pairs(watched) do
    local info = creature:valid() and creature:get()
    if not info then
      kyubu.info(("%s (id %d) is gone"):format(w.label, w.id))
      watched[creature] = nil
    else
      local dx, dy = (info.pos.x - pinfo.pos.x) / UNITS_PER_BLOCK, (info.pos.y - pinfo.pos.y) / UNITS_PER_BLOCK
      line[#line + 1] = ("%s: hp %.1f, %.1f east, %.1f north of the player"):format(w.label:match("^%a+"), info.hp, dx, dy)
    end
  end
  if #line > 1 then kyubu.info(table.concat(line, " | ")) end
end)

-- ---------------------------------------------------------------------------------------------------------------------
-- Task 8.2 (probe spawn-behavior-all): every behaviour class attached by hand, one per friendly slime (hostile type 3,
-- which the player cannot hurt), then a hostile slime with a lone CombatBehavior (no sequence). Messages:
-- `behavior_proto:cases` { kind = "all" | "combat" | "kill" }.
-- ---------------------------------------------------------------------------------------------------------------------
local NAMED_FRIENDLY = 3

-- The block is zeroed first: the constructors leave some fields unwritten (WalkPath +0x14, Companion +4), and the game
-- normally reaches a creature through a clone (the copy constructors write those fields), so the first probe runs read
-- heap garbage there.
local function raw_new(name, size, ...)
  local block = fn("operator_new")(size)
  ffi.fill(block, size, 0)
  return fn(name)(block, nil, ...)
end

local build = {
  combat      = function() return raw_new("cube::CombatBehavior::CombatBehavior", 20, 20.0) end,
  look_at     = function() return raw_new("cube::LookAtPlayerBehavior::LookAtPlayerBehavior", 4) end,
  walk        = function() return raw_new("cube::RandomWalkBehavior::RandomWalkBehavior", 8) end,
  spawn_loc   = function() return raw_new("cube::SpawnLocationBehavior::SpawnLocationBehavior", 4) end,
  interaction = function() return raw_new("cube::RandomInteractionBehavior::RandomInteractionBehavior", 8) end,
}

-- The owner is the creature id, a uint64 at +8 (the constructor zeroes it; the pet and summon paths write it by hand).
function build.companion(owner_id)
  local obj = raw_new("cube::CompanionBehavior::CompanionBehavior", 16)
  ffi.cast("int64_t*", ffi.cast("uintptr_t", obj) + 8)[0] = owner_id
  return obj
end

-- A path: the constructor's float, then the waypoints (LongVector3, world units) appended to the vector at +4.
function build.path(points, value)
  local obj = raw_new("cube::WalkPathBehavior::WalkPathBehavior", 28, value)
  local vec = ffi.cast("void*", ffi.cast("uintptr_t", obj) + 4)
  local push = fn("WaypointVector::push_back")
  local p = ffi.new("cube_LongVector3[1]")
  for _, pt in ipairs(points) do
    p[0].x, p[0].y, p[0].z = pt.x, pt.y, pt.z
    push(vec, nil, p)
  end
  return obj
end

local function sequence_of(children)
  local seq = raw_new("cube::SequentialBehavior::SequentialBehavior", 12)
  local list = ffi.cast("void*", ffi.cast("uintptr_t", seq) + 4)
  local push_back = fn("std::list::push_back")
  local cell = ffi.new("void*[1]")
  for _, child in ipairs(children) do
    cell[0] = ffi.cast("void*", child)
    push_back(list, nil, cell)
  end
  return seq
end

-- Words of an object after its vtable, for the state lines (the class sizes are 4 to 28 bytes).
local function words(obj, size)
  local out = {}
  local p = ffi.cast("uint32_t*", obj)
  for i = 1, size / 4 - 1 do out[#out + 1] = ("%X"):format(p[i]) end
  return table.concat(out, " ")
end

local case_list = {}   -- { label =, creature = handle, id =, objs = { { name =, obj =, size = } }, spawn = {x =, y =}, kill_at = }

local function add_case(label, hostile, level, at, objs, attach, kill_after)
  local creature, why = kyubu.game.creature_spawn({
    type = SLIME, hostile = hostile, level = level, pos = at, yaw = 0 })
  if not creature then return kyubu.warn(("%s: not spawned: %s"):format(label, tostring(why))) end
  local info = creature:get()
  local raw = creature_by_id(info.id)
  if not raw then return kyubu.warn(label .. ": the creature is not in the map") end
  raw.data.spawn_pos.x, raw.data.spawn_pos.y, raw.data.spawn_pos.z = raw.data.pos.x, raw.data.pos.y, raw.data.pos.z
  local text = ("%s: id %d, hostile %d at %.1f %.1f"):format(label, info.id, info.hostile,
    info.pos.x / UNITS_PER_BLOCK, info.pos.y / UNITS_PER_BLOCK)
  if attach then
    if raw.behavior ~= nil then return kyubu.warn(label .. ": the creature has a behaviour already") end
    local ok, top = pcall(attach)
    if not ok then return kyubu.warn(("%s: behaviour failed: %s"):format(label, tostring(top))) end
    raw.behavior = ffi.cast("cube_Behavior*", top)
    text = ("%s; behaviour %s"):format(text, tostring(top))
  end
  kyubu.info(text)
  case_list[#case_list + 1] = { label = label, creature = creature, id = info.id, objs = objs or {},
    spawn = { x = info.pos.x, y = info.pos.y }, kill_at = kill_after and (os.time() + kill_after) or nil }
end

local function blocks(v) return v * UNITS_PER_BLOCK end

local function spawn_all(m)
  local level = math.max(1, tonumber(m.level) or 1)
  local function at(dx, dy) return { x = m.x + blocks(dx), y = m.y + blocks(dy), z = m.z } end
  local function obj(name, o, size) return { { name = name, obj = o, size = size } } end
  add_case("control (no behaviour)", NAMED_FRIENDLY, level, at(-8, 6))
  local w = build.walk()
  add_case("randomwalk (lone, not in a sequence)", NAMED_FRIENDLY, level, at(8, 6), obj("RandomWalk", w, 8),
    function() return w end)
  local sl, w2 = build.spawn_loc(), build.walk()
  add_case("spawnloc (SpawnLocation, RandomWalk)", NAMED_FRIENDLY, level, at(-8, -6),
    { { name = "SpawnLocation", obj = sl, size = 4 }, { name = "RandomWalk", obj = w2, size = 8 } },
    function() return sequence_of({ sl, w2 }) end)
  local la = build.look_at()
  add_case("lookat (LookAtPlayer)", NAMED_FRIENDLY, level, at(0, 10), obj("LookAt", la, 4),
    function() return sequence_of({ la }) end)
  local ri = build.interaction()
  add_case("interaction (RandomInteraction)", NAMED_FRIENDLY, level, at(8, -6), obj("Interaction", ri, 8),
    function() return sequence_of({ ri }) end)
  local sx, sy = m.x + blocks(16), m.y - blocks(2)
  local pts = { { x = sx, y = sy, z = m.z }, { x = sx, y = sy + blocks(5), z = m.z },
                { x = sx + blocks(5), y = sy + blocks(5), z = m.z }, { x = sx + blocks(5), y = sy, z = m.z } }
  local wp = build.path(pts, 2.0)
  add_case("path (WalkPath square, 5 blocks)", NAMED_FRIENDLY, level, { x = sx, y = sy, z = m.z }, obj("WalkPath", wp, 28),
    function() return sequence_of({ wp }) end)
  local cb, co = build.combat(), build.companion(m.player_id)
  add_case("companion (Combat, Companion of the player)", NAMED_FRIENDLY, level, at(0, -12),
    { { name = "Combat", obj = cb, size = 20 }, { name = "Companion", obj = co, size = 16 } },
    function() return sequence_of({ cb, co }) end)
  -- the same look-at, path and interaction classes close to the player (the far ones did nothing in the first run)
  local la2 = build.look_at()
  add_case("lookat-near (LookAtPlayer, 4 blocks north)", NAMED_FRIENDLY, level, at(0, 4), obj("LookAt", la2, 4),
    function() return sequence_of({ la2 }) end)
  local ri2 = build.interaction()
  add_case("interaction-near (RandomInteraction, 4 blocks west)", NAMED_FRIENDLY, level, at(-4, 0),
    obj("Interaction", ri2, 8), function() return sequence_of({ ri2 }) end)
  local nx, ny = m.x + blocks(4), m.y - blocks(4)
  local npts = { { x = nx, y = ny, z = m.z }, { x = nx, y = ny + blocks(3), z = m.z },
                 { x = nx + blocks(3), y = ny + blocks(3), z = m.z }, { x = nx + blocks(3), y = ny, z = m.z } }
  local wp2 = build.path(npts, 2.0)
  add_case("path-near (WalkPath square, 3 blocks, 4 blocks away)", NAMED_FRIENDLY, level, { x = nx, y = ny, z = m.z },
    obj("WalkPath", wp2, 28), function() return sequence_of({ wp2 }) end)
  -- the game's shapes: a guard is Seq[Combat, WalkPath], and every generated path holds one waypoint (the spawn's own)
  local gx, gy = m.x + blocks(5), m.y - blocks(9)
  local gpts = { { x = gx, y = gy, z = m.z }, { x = gx, y = gy + blocks(3), z = m.z },
                 { x = gx + blocks(3), y = gy + blocks(3), z = m.z }, { x = gx + blocks(3), y = gy, z = m.z } }
  local gc, gw = build.combat(), build.path(gpts, 2.0)
  add_case("path-combat (Seq[Combat, WalkPath], the guard shape)", NAMED_FRIENDLY, level, { x = gx, y = gy, z = m.z },
    { { name = "Combat", obj = gc, size = 20 }, { name = "WalkPath", obj = gw, size = 28 } },
    function() return sequence_of({ gc, gw }) end)
  local px, py = m.x - blocks(3), m.y - blocks(3)
  local pw = build.path({ { x = px, y = py - blocks(6), z = m.z } }, 2.0)
  add_case("path-post (one waypoint 6 blocks south of its spawn)", NAMED_FRIENDLY, level, { x = px, y = py, z = m.z },
    obj("WalkPath", pw, 28), function() return sequence_of({ pw }) end)
end

-- The phases run on the mod's own clock (the console steps of a replay land 8 to 60 s late): the friendly cases are
-- killed KILL_AFTER seconds after the message except the companion, the hostile one is spawned COMBAT_AFTER seconds
-- after it and killed 35 s later, and the companion 10 s after that.
local KILL_AFTER, COMBAT_AFTER = 170, 200
local schedule = nil   -- { kill_at =, combat_at =, level = }

kyubu.channel.register("behavior_proto:cases", function(_, m)
  if type(m) ~= "table" or m.kind ~= "all" then return end
  kyubu.info(("cases: player id %s"):format(tostring(m.player_id)))
  spawn_all(m)
  local now = os.time()
  schedule = { kill_at = now + KILL_AFTER, combat_at = now + COMBAT_AFTER, companion_kill_at = now + COMBAT_AFTER + 45, level = math.max(1, tonumber(m.level) or 1) }
end)

local function run_schedule(now)
  if not schedule then return end
  if schedule.kill_at and now >= schedule.kill_at then
    schedule.kill_at = nil
    kyubu.info("cases: phase kill")
    for _, c in ipairs(case_list) do
      local raw = creature_by_id(c.id)
      -- the companion stays for the combat phase: does it fight what the player fights?
      if raw and not c.label:find("^companion") then raw.data.hp = 0 end
    end
  end
  if schedule.companion_kill_at and now >= schedule.companion_kill_at then
    schedule.companion_kill_at = nil
    for _, c in ipairs(case_list) do
      local raw = creature_by_id(c.id)
      if raw and c.label:find("^companion") then raw.data.hp = 0 end
    end
    kyubu.info("cases: phase companion kill")
  end
  if schedule.combat_at and now >= schedule.combat_at then
    schedule.combat_at = nil
    local pinfo = player_state()
    local gc = ffi.cast("uintptr_t*", assert(kyubu.symbol("cube::game_controller")))[0]
    local player = ffi.cast("cube_Creature**", gc + 0x8006D0)[0]
    if pinfo and player ~= nil then
      kyubu.info("cases: phase combat")
      local cb = build.combat()
      add_case("combat (a lone CombatBehavior, hostile)", HOSTILE, schedule.level,
        { x = tonumber(player.data.pos.x) + blocks(6), y = tonumber(player.data.pos.y) + blocks(6),
          z = tonumber(player.data.pos.z) + blocks(1) },
        { { name = "Combat", obj = cb, size = 20 } }, function() return cb end, 35)
    else
      kyubu.warn("cases: no player for the combat phase")
    end
  end
end

local case_ticks = 0
kyubu.on("sim.tick", function()
  case_ticks = case_ticks + 1
  run_schedule(os.time())
  if case_ticks % 40 ~= 0 or #case_list == 0 then return end
  local now = os.time()
  local pinfo = player_state()
  local ppos = pinfo and pinfo.pos
  if pinfo then kyubu.info(("cases t=%d: player hp %.1f at %.1f %.1f"):format(now, pinfo.hp,
    ppos.x / UNITS_PER_BLOCK, ppos.y / UNITS_PER_BLOCK)) end
  for i = #case_list, 1, -1 do
    local c = case_list[i]
    local info = c.creature:valid() and c.creature:get()
    if not info then
      kyubu.info(("cases t=%d: %s (id %d) is gone"):format(now, c.label, c.id))
      table.remove(case_list, i)
    else
      if c.kill_at and now >= c.kill_at then
        local raw = creature_by_id(c.id)
        if raw then raw.data.hp = 0 end
        kyubu.info(("cases t=%d: %s: hp 0 written"):format(now, c.label))
      end
      local bx, by = info.pos.x / UNITS_PER_BLOCK, info.pos.y / UNITS_PER_BLOCK
      local text = ("cases t=%d: %s: at %.1f %.1f, %.1f from its spawn, yaw %.1f, hp %.1f"):format(now, c.label, bx, by,
        math.sqrt((bx - c.spawn.x / UNITS_PER_BLOCK) ^ 2 + (by - c.spawn.y / UNITS_PER_BLOCK) ^ 2), info.yaw, info.hp)
      if ppos then
        local dx, dy = ppos.x / UNITS_PER_BLOCK - bx, ppos.y / UNITS_PER_BLOCK - by
        text = ("%s; player %.1f away, bearing to it %.0f (0 north, 90 east)"):format(text, math.sqrt(dx * dx + dy * dy),
          math.deg(math.atan2(dx, dy)) % 360)
      end
      for _, o in ipairs(c.objs) do text = ("%s; %s words %s"):format(text, o.name, words(o.obj, o.size)) end
      kyubu.info(text)
    end
  end
end)
