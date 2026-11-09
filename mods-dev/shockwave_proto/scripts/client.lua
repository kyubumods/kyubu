-- shockwave_proto, client side (probe shockwave-ring). A scripted driver for a replay: it makes shockwaves at known places round the
-- player and logs a marker before each stage. A shockwave that does not expand (speed 0) and lives long holds still for a picture.
local ffi = require("ffi")
local BLOCK = 65536

pcall(ffi.cdef, [[
typedef struct EmShockwaveDef {
  uint32_t size; uint32_t flags;
  int64_t pos[3];
  float facing[3];
  float inner_radius, thickness, expansion;
  uint32_t life_ticks, fade_on_tick;
  float fade_speed;
  float color[4];
} EmShockwaveDef;
typedef struct EmShockwaveApi {
  uint32_t size, version;
  uint32_t (*spawn)(const EmShockwaveDef*);
  int (*cancel)(uint32_t);
  uint32_t (*count)(void);
  void (*clear)(void);
} EmShockwaveApi;
]])

local function api()
  local p = kyubu.interface("emtweaks:shockwave", 1)
  return p and ffi.cast("EmShockwaveApi*", p) or nil
end

local function ring(sw, at, dx, dy, dz, facing, inner, thick, speed, life, fade_on, fade, r, g, b, a)
  local d = ffi.new("EmShockwaveDef")
  d.size = ffi.sizeof(d)
  d.pos[0], d.pos[1], d.pos[2] = at.x + dx * BLOCK, at.y + dy * BLOCK, at.z + dz * BLOCK
  d.facing[0], d.facing[1], d.facing[2] = facing[1], facing[2], facing[3]
  d.inner_radius, d.thickness, d.expansion = inner, thick, speed
  d.life_ticks, d.fade_on_tick, d.fade_speed = life, fade_on, fade
  d.color[0], d.color[1], d.color[2], d.color[3] = r, g, b, a
  return sw.spawn(d)
end

-- The player drops in from above while the world loads: wait until its height has held for two seconds
local t, stage = 0, 0
local last_z, still_since = nil, 0
kyubu.on("frame.begin", function(ev)
  t = t + (ev.dt or 0) / 1000
  local c = kyubu.game and kyubu.game.creature_player()
  if not c then return end
  local info = c:get()
  if not info then return end
  if last_z == nil or math.abs(info.pos.z - last_z) > 0.05 * BLOCK then
    last_z, still_since = info.pos.z, t
  end
  if stage == 1 and (info.pos.z < 1 * BLOCK or t - still_since < 2.0) then return end
  local sw = api()
  if not sw then
    if stage == 0 then kyubu.warn("shockwave-proto: no interface"); stage = -1 end
    return
  end
  if stage == 0 then
    stage = 1
    t = 0
    kyubu.info("shockwave-proto: ready")
  elseif stage == 1 then
    stage = 2
    local p = info.pos
    kyubu.info(("shockwave-proto: static player at %.2f %.2f %.2f blocks, yaw %.1f"):format(p.x / BLOCK, p.y / BLOCK, p.z / BLOCK, info.yaw))
    sw.clear()
    -- Four rings round the player, each in another plane and at another radius, like a gyroscope; the player stands in the middle.
    -- orange: flat on the ground, round the player's feet
    ring(sw, p, 0, 0, 0.1, { 0, 0, 1 }, 3.3, 0.5, 0, 3000, 3000, 0, 1.0, 0.5, 0.1, 0.9)
    -- cyan: on its edge, facing east, round the player's chest
    ring(sw, p, 0, 0, 1.5, { 1, 0, 0 }, 1.8, 0.3, 0, 3000, 3000, 0, 0.1, 0.9, 1.0, 0.9)
    -- green: on its edge, facing north
    ring(sw, p, 0, 0, 1.5, { 0, 1, 0 }, 2.3, 0.3, 0, 3000, 3000, 0, 0.2, 1.0, 0.2, 0.9)
    -- magenta: tilted 45 degrees
    ring(sw, p, 0, 0, 1.5, { 0.5, 0.5, 0.7 }, 2.8, 0.3, 0, 3000, 3000, 0, 1.0, 0.2, 1.0, 0.9)
  elseif stage == 2 and t > 10 then
    -- DIAGNOSIS (replay shockwave-ring): the rings one at a time, to tell a ring that is not drawn from one that is hidden by another
    stage = 3
    t = 0
    local p = info.pos
    sw.clear()
    kyubu.info("shockwave-proto: solo-green")
    ring(sw, p, 0, 0, 1.5, { 0, 1, 0 }, 2.3, 0.3, 0, 3000, 3000, 0, 0.2, 1.0, 0.2, 0.9)
  elseif stage == 3 and t > 10 then
    stage = 4
    t = 0
    local p = info.pos
    sw.clear()
    kyubu.info("shockwave-proto: solo-yellow")
    ring(sw, p, 0, 0, 1.5, { 0, 0, 1 }, 1.0, 0.3, 0, 3000, 3000, 0, 1.0, 1.0, 0.1, 0.9)
  elseif stage == 4 and t > 10 then
    stage = 5
    t = 0
    local p = info.pos
    sw.clear()
    kyubu.info("shockwave-proto: pair")
    ring(sw, p, 0, 0, 0.1, { 0, 0, 1 }, 3.3, 0.5, 0, 3000, 3000, 0, 1.0, 0.5, 0.1, 0.9)
    ring(sw, p, 0, 0, 0.1, { 0, 0, 1 }, 5.0, 0.5, 0, 3000, 3000, 0, 0.1, 0.1, 1.0, 0.9)
  elseif stage == 5 and t > 10 then
    stage = 6
    t = 0
    local p = info.pos
    sw.clear()
    kyubu.info("shockwave-proto: expanding")
    -- orange, expanding on the ground and fading: 0.03 block a tick for 600 ticks (12 s of game time)
    ring(sw, p, 0, 0, 0.1, { 0, 0, 1 }, 0.5, 0.6, 0.03, 600, 450, 0.01, 1.0, 0.5, 0.1, 0.9)
  elseif stage == 6 and t > 20 then
    stage = 7
    sw.clear()
    kyubu.info("shockwave-proto: done")
  end
end)
