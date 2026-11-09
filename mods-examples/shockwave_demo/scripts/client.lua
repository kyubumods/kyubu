-- shockwave_demo: a fourth ability for the warrior (slot 4, key 4) that only draws, and console commands that try the effects of EMTweaks
-- (mod id emtweaks): a ring flat on the ground, a ring on its edge in front of the player, a tilted ring, rings in a pulse, and the game's own
-- particles by preset.
--
-- EMTweaks' effects come through two interfaces, `emtweaks:shockwave` and `emtweaks:particles`. Each is a table of C functions: the mod needs
-- the FFI, so its trust is full. Positions are in world units (65,536 to a block; x east, y north, z up), like creature_get's.
local ffi = require("ffi")

local ID = "shockwave_demo:wave"
local BLOCK = 65536

-- The interfaces' types (api/emtweaks.h of the mod). Another mod in this VM may have declared them already.
pcall(ffi.cdef, [[
typedef struct EmShockwaveDef {
  uint32_t size; uint32_t flags;
  int64_t pos[3];
  float facing[3];
  float inner_radius, thickness, expansion;
  uint32_t life_ticks, fade_on_tick;
  float fade_speed;
  float color[4];
  float pixel_size;
} EmShockwaveDef;
typedef struct EmShockwaveApi {
  uint32_t size, version;
  uint32_t (*spawn)(const EmShockwaveDef*);
  int (*cancel)(uint32_t);
  uint32_t (*count)(void);
  void (*clear)(void);
} EmShockwaveApi;
typedef struct EmParticleDef {
  uint32_t size; uint32_t type;
  int64_t pos[3];
  float accel[3];
  float color[4];
  float scale;
  uint32_t count;
  float spreading;
} EmParticleDef;
typedef struct EmParticleRingDef {
  uint32_t size; uint32_t type;
  int64_t pos[3];
  float facing[3];
  float radius;
  uint32_t points;
  float time;
  float accel[3];
  float color[4];
  float scale;
  uint32_t count;
  float spreading;
} EmParticleRingDef;
typedef struct EmParticleApi {
  uint32_t size, version;
  int (*spawn)(const EmParticleDef*);
  int (*spawn_preset)(const char* name, const int64_t* pos, const float* color, float scale_mul);
  int (*define_preset)(const char* name, const EmParticleDef* def);
  int (*preset_get)(const char* name, EmParticleDef* out);
  uint32_t (*preset_count)(void);
  const char* (*preset_name)(uint32_t index);
  int (*spawn_ring)(const EmParticleRingDef*);
} EmParticleApi;
]])

local sw_api, pa_api = nil, nil
local function shockwave_api()
  if sw_api then return sw_api end
  local p = kyubu.interface("emtweaks:shockwave", 1)
  if not p then return nil end
  sw_api = ffi.cast("EmShockwaveApi*", p)
  return sw_api
end
local function particle_api()
  if pa_api then return pa_api end
  local p = kyubu.interface("emtweaks:particles", 1)
  if not p then return nil end
  pa_api = ffi.cast("EmParticleApi*", p)
  return pa_api
end

-- One ring. `at` is {x, y, z} in world units; `facing` {x, y, z} (any length; all zero is up).
local function ring(at, facing, inner, thick, speed, life, fade_on, fade, r, g, b, a)
  local sw = shockwave_api()
  if not sw then return 0 end
  local d = ffi.new("EmShockwaveDef")
  d.size = ffi.sizeof(d)
  d.pos[0], d.pos[1], d.pos[2] = at[1], at[2], at[3]
  d.facing[0], d.facing[1], d.facing[2] = facing[1], facing[2], facing[3]
  d.inner_radius, d.thickness, d.expansion = inner, thick, speed
  d.life_ticks, d.fade_on_tick, d.fade_speed = life, fade_on, fade
  d.color[0], d.color[1], d.color[2], d.color[3] = r, g, b, a
  return sw.spawn(d)
end

-- The game's own particles: `name` is a preset of EMTweaks ("vanilla.hit"); `color` is {r, g, b, a} or nil; `scale_mul` or nil.
local function preset(name, at, color, scale_mul)
  local pa = particle_api()
  if not pa then return false end
  local pos = ffi.new("int64_t[3]", at[1], at[2], at[3])
  local col = nil
  if color then col = ffi.new("float[4]", color[1], color[2], color[3], color[4]) end
  return pa.spawn_preset(name, pos, col, scale_mul or 1.0) == 1
end

-- One effect of the game's particles with every value given.
local function particles(at, t, count, scale, spreading, accel, color)
  local pa = particle_api()
  if not pa then return false end
  local d = ffi.new("EmParticleDef")
  d.size = ffi.sizeof(d)
  d.type, d.count, d.scale, d.spreading = t, count, scale, spreading
  d.pos[0], d.pos[1], d.pos[2] = at[1], at[2], at[3]
  d.accel[0], d.accel[1], d.accel[2] = accel[1], accel[2], accel[3]
  d.color[0], d.color[1], d.color[2], d.color[3] = color[1], color[2], color[3], color[4]
  return pa.spawn(d) == 1
end

-- A ring of the game's particles round `at` (world units): they start on a circle of `radius` blocks and gather to the centre in `time`
-- seconds (a negative time spreads them out from it). `facing` {x, y, z} is the ring's normal. Returns how many effects were queued.
local function particle_ring(at, facing, radius, points, time, t, count, scale, color)
  local pa = particle_api()
  if not pa then return 0 end
  local d = ffi.new("EmParticleRingDef")
  d.size = ffi.sizeof(d)
  d.type, d.count, d.scale, d.spreading = t, count, scale, 0
  d.pos[0], d.pos[1], d.pos[2] = at[1], at[2], at[3]
  d.facing[0], d.facing[1], d.facing[2] = facing[1], facing[2], facing[3]
  d.radius, d.points, d.time = radius, points, time
  d.color[0], d.color[1], d.color[2], d.color[3] = color[1], color[2], color[3], color[4]
  return pa.spawn_ring(d)
end

-- The player's position (world units) and the direction it looks (unit x, y), or nil and the reason.
local function pose()
  local player, why = kyubu.game.creature_player()
  if not player then return nil, why end
  local info = player:get()
  if not info then return nil, "the player cannot be read" end
  local yaw = math.rad(info.yaw or 0)
  -- yaw is counter-clockwise from north (+y): forward is (-sin, cos)
  return { info.pos.x, info.pos.y, info.pos.z }, { -math.sin(yaw), math.cos(yaw), 0 }
end

local function player_pose(ctx)
  local info = ctx.creature and ctx.creature:get()
  if not info then return nil end
  local yaw = math.rad(info.yaw or 0)
  return { info.pos.x, info.pos.y, info.pos.z }, { -math.sin(yaw), math.cos(yaw), 0 }
end

local function burst(at)
  -- the game's "bomb" particles in the colours of the ring
  particles(at, 1, 24, 0.35, 6.0, { 0, 0, 10 }, { 1.0, 0.55, 0.1, 1.0 })
end

kyubu.on("lifecycle.register", function()
  if not kyubu.ability then kyubu.info("shockwave_demo: this loader has no kyubu.ability"); return end
  local ok, err = kyubu.ability.define(ID, {
    icon = "cubeworld:smash",
    duration_ms = 700,
    mp_cost = 0,
    max_rank = 3,
    cooldown_ms = function(rank) return 3000 end,
    claim = { slot = 4, class = "warrior", specialization = "any" },

    on_start = function(ctx)
      if not shockwave_api() then
        kyubu.warn("shockwave_demo: the interface emtweaks:shockwave is not there: is the EMTweaks mod loaded?")
        return
      end
      local at, fwd = player_pose(ctx)
      if not at then return end
      local feet = { at[1], at[2], at[3] + 0.1 * BLOCK }
      -- 1. flat on the ground, fast and fading: the classic shockwave
      ring(feet, { 0, 0, 1 }, 0.5, 0.7, 0.35, 36, 12, 0.045, 1.0, 0.55, 0.1, 0.85)
      -- 3. a ring on its edge, one block above the ground in front of the player, facing the way the player looks
      local front = { at[1] + fwd[1] * 2.5 * BLOCK, at[2] + fwd[2] * 2.5 * BLOCK, at[3] + 1.6 * BLOCK }
      ring(front, fwd, 0.3, 0.35, 0.18, 30, 8, 0.05, 0.2, 0.9, 1.0, 0.9)
      -- 4. a tilted ring that leaves at a slant (45 degrees up, towards the player's right)
      local tilt = { fwd[2] * 0.5 + fwd[1] * 0.5, -fwd[1] * 0.5 + fwd[2] * 0.5, 0.7 }
      ring({ at[1], at[2], at[3] + 1.0 * BLOCK }, tilt, 0.4, 0.3, 0.2, 40, 10, 0.04, 0.9, 0.3, 1.0, 0.85)
      burst({ at[1], at[2], at[3] + 0.5 * BLOCK })
      ctx.state.second = false
      kyubu.info(("shockwave_demo: on_start rank %d, %d shockwave(s) alive"):format(ctx.rank, tonumber(shockwave_api().count())))
    end,

    on_step = function(ctx)
      -- 2. a second, thin white ring starts a little later and catches up with the first
      if not ctx.state.second and ctx.elapsed_ms >= 120 then
        ctx.state.second = true
        local at = player_pose(ctx)
        if at then ring({ at[1], at[2], at[3] + 0.1 * BLOCK }, { 0, 0, 1 }, 0.5, 0.25, 0.5, 30, 6, 0.06, 1.0, 1.0, 1.0, 0.9) end
      end
    end,
  })
  kyubu.info(("shockwave_demo: define: %s"):format(ok and "ok" or tostring(err)))
end)

-- ---- console commands ------------------------------------------------------------------------------------------------------
-- Each one needs a world (it reads the player) and EMTweaks.
local function need(print)
  if not (shockwave_api() and particle_api()) then
    print("the interfaces of EMTweaks are not there: is the mod loaded (client side)?")
    return nil
  end
  local at, fwd = pose()
  if not at then
    print("no player: " .. tostring(fwd))
    return nil
  end
  return at, fwd
end

local function number(args, default, lo, hi)
  local n = tonumber((args or ""):match("%S+"))
  if not n then return default end
  return math.max(lo, math.min(hi, n))
end

-- Rings that start later: { at_ms, fn } entries run from frame.begin.
local later = {}
local clock = 0
kyubu.on("frame.begin", function(ev)
  clock = clock + (ev and ev.dt or 16)
  local i = 1
  while i <= #later do
    if clock >= later[i].at then
      local job = table.remove(later, i)
      job.fn()
    else
      i = i + 1
    end
  end
end)

-- shockwave_demo:nova [rings]: rings of every speed start together on the ground; the fast ones leave the slow ones behind.
kyubu.command("shockwave_demo:nova", "[rings 1-8, default 4]: rings of different speeds start together on the ground", function(args, print)
  local at = need(print)
  if not at then return end
  local n = math.floor(number(args, 4, 1, 8))
  local feet = { at[1], at[2], at[3] + 0.1 * BLOCK }
  for i = 1, n do
    local t = (i - 1) / math.max(1, n - 1)
    ring(feet, { 0, 0, 1 }, 0.4, 0.35, 0.12 + 0.3 * t, 40, 14, 0.04, 1.0 - 0.8 * t, 0.4 + 0.4 * t, 0.2 + 0.8 * t, 0.9)
  end
  print(("shockwave_demo: nova of %d ring(s)"):format(n))
end)

-- shockwave_demo:pulse [rings]: one ring every 0.25 s from the player.
kyubu.command("shockwave_demo:pulse", "[rings 1-12, default 5]: one ground ring every 0.25 s", function(args, print)
  local at = need(print)
  if not at then return end
  local n = math.floor(number(args, 5, 1, 12))
  for i = 1, n do
    later[#later + 1] = {
      at = clock + (i - 1) * 250,
      fn = function()
        local here = pose()   -- the ring starts where the player is now
        if here then ring({ here[1], here[2], here[3] + 0.1 * BLOCK }, { 0, 0, 1 }, 0.4, 0.3, 0.25, 32, 10, 0.05, 0.3, 1.0, 0.6, 0.85) end
      end,
    }
  end
  print(("shockwave_demo: pulse of %d ring(s) over %.1f s"):format(n, (n - 1) * 0.25))
end)

-- shockwave_demo:gyro: three rings that stand on each other's axes round the player's chest: east-west, north-south and flat.
kyubu.command("shockwave_demo:gyro", "three rings at right angles round the player's chest", function(_, print)
  local at = need(print)
  if not at then return end
  local c = { at[1], at[2], at[3] + 1.2 * BLOCK }
  ring(c, { 1, 0, 0 }, 0.8, 0.15, 0.08, 50, 20, 0.04, 1.0, 0.2, 0.2, 0.9)
  ring(c, { 0, 1, 0 }, 0.8, 0.15, 0.08, 50, 20, 0.04, 0.2, 1.0, 0.2, 0.9)
  ring(c, { 0, 0, 1 }, 0.8, 0.15, 0.08, 50, 20, 0.04, 0.3, 0.4, 1.0, 0.9)
  print("shockwave_demo: gyro (red faces east, green north, blue up)")
end)

-- shockwave_demo:tower [rings]: flat rings stacked up from the ground, each wider than the one under it: a funnel.
kyubu.command("shockwave_demo:tower", "[rings 2-12, default 6]: a funnel of flat rings, one block-ish apart", function(args, print)
  local at = need(print)
  if not at then return end
  local n = math.floor(number(args, 6, 2, 12))
  for i = 0, n - 1 do
    ring({ at[1], at[2], at[3] + (0.2 + 0.5 * i) * BLOCK }, { 0, 0, 1 }, 0.4 + 0.25 * i, 0.3, 0.06, 45, 20, 0.04, 0.9, 0.9 - 0.06 * i, 0.2 + 0.1 * i, 0.85)
  end
  print(("shockwave_demo: tower of %d ring(s)"):format(n))
end)

-- shockwave_demo:burst [type]: the game's own particles at the player's chest. One type, or all five in a row when none is given.
kyubu.command("shockwave_demo:burst", "[type 0-4]: the game's particles by type; no type makes all five in a row", function(args, print)
  local at, fwd = need(print)
  if not at then return end
  local only = tonumber((args or ""):match("%S+"))
  local right = { fwd[2], -fwd[1] }
  for t = 0, 4 do
    if only == nil or only == t then
      local side = only == nil and (t - 2) * 1.5 or 0
      local p = { at[1] + (fwd[1] * 3 + right[1] * side) * BLOCK, at[2] + (fwd[2] * 3 + right[2] * side) * BLOCK, at[3] + 1.2 * BLOCK }
      particles(p, t, 14, 0.25, 3.0, { 0, 0, 0 }, { 1.0, 1.0, 1.0, 1.0 })
    end
  end
  print(only == nil and "shockwave_demo: types 0 to 4 left to right, 3 blocks ahead" or ("shockwave_demo: type %d, 3 blocks ahead"):format(only))
end)

-- shockwave_demo:combo: a ground ring, a wall ring, and a burst of the game's hit effect on each, together.
kyubu.command("shockwave_demo:combo", "a ring and a hit effect together, on the ground and in front of the player", function(_, print)
  local at, fwd = need(print)
  if not at then return end
  local feet = { at[1], at[2], at[3] + 0.1 * BLOCK }
  ring(feet, { 0, 0, 1 }, 0.5, 0.6, 0.3, 36, 12, 0.045, 1.0, 0.8, 0.2, 0.9)
  local ahead = { at[1] + fwd[1] * 4 * BLOCK, at[2] + fwd[2] * 4 * BLOCK, at[3] + 0.3 * BLOCK }
  ring(ahead, { 0, 0, 1 }, 0.2, 0.4, 0.2, 30, 8, 0.05, 1.0, 0.3, 0.2, 0.9)
  local ok1 = preset("vanilla.hit", feet, { 1.0, 0.8, 0.2, 1.0 }, 1.0)
  local ok2 = preset("vanilla.hit", ahead, { 1.0, 0.3, 0.2, 1.0 }, 1.6)
  print(("shockwave_demo: combo (hit effects %s)"):format((ok1 and ok2) and "started" or "refused: see the log"))
end)

-- shockwave_demo:implode [seconds]: particles gather to the player's chest from a ring and a ring of light shrinks with them, then the
-- ring bursts out as a shockwave: a charge-up. The particle ring needs the particle's units to hold (accel in blocks per second squared).
kyubu.command("shockwave_demo:implode", "[seconds 0.3-5, default 1.2]: particles gather from a ring while a ring shrinks, then a shockwave", function(args, print)
  local at = need(print)
  if not at then return end
  local secs = number(args, 1.2, 0.3, 5)
  local c = { at[1], at[2], at[3] + 1.0 * BLOCK }
  local n = particle_ring(c, { 0, 0, 1 }, 3.0, 24, secs, 4, 1, 0.2, { 0.5, 0.8, 1.0, 1.0 })
  -- a shrinking ring: 3 blocks to 0 in `secs` seconds (50 ticks a second)
  local ticks = math.floor(secs * 50)
  ring(c, { 0, 0, 1 }, 2.9, 0.15, -2.9 / ticks, ticks, 0, 0, 0.5, 0.8, 1.0, 0.8)
  later[#later + 1] = {
    at = clock + secs * 1000,
    fn = function()
      local here = pose()
      if here then ring({ here[1], here[2], here[3] + 0.1 * BLOCK }, { 0, 0, 1 }, 0.3, 0.5, 0.4, 30, 8, 0.05, 0.6, 0.9, 1.0, 0.9) end
    end,
  }
  print(("shockwave_demo: implode over %.1f s (%d particle point(s))"):format(secs, n))
end)

-- shockwave_demo:halo [type]: a standing ring of particles that spreads out from the player's chest, facing where the player looks.
kyubu.command("shockwave_demo:halo", "[type 0-4, default 4]: a ring of particles that spreads out in front of the player, facing the player's way", function(args, print)
  local at, fwd = need(print)
  if not at then return end
  local t = math.floor(number(args, 4, 0, 4))
  local c = { at[1] + fwd[1] * 2.5 * BLOCK, at[2] + fwd[2] * 2.5 * BLOCK, at[3] + 1.5 * BLOCK }
  local n = particle_ring(c, fwd, 0.5, 20, -1.0, t, 1, 0.2, { 1.0, 0.9, 0.3, 1.0 })
  print(("shockwave_demo: halo of %d point(s), type %d"):format(n, t))
end)

kyubu.command("shockwave_demo:clear", "ends every shockwave", function(_, print)
  local sw = shockwave_api()
  if not sw then return print("EMTweaks is not loaded") end
  sw.clear()
  later = {}
  print("shockwave_demo: shockwaves ended")
end)
