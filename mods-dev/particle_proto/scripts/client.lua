-- particle_proto, client side (probe particle-types). A scripted driver for a replay: rows of particle effects at known places in front
-- of the player, made again every half second for a few seconds so that a picture catches them.
local BLOCK = 65536

local t, stage, next_spawn, count = 0, 0, 0, 0
local rows = nil

local function spawn(at, dx, dy, dz, p)
  p.pos = { at.x + dx * BLOCK, at.y + dy * BLOCK, at.z + dz * BLOCK }
  local ok, why = kyubu.game.particle_spawn(p)
  if not ok then kyubu.warn("particle-proto: refused: " .. tostring(why)) end
end

-- The player drops in from above while the world loads: wait until its height has held for two seconds
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
  if not (kyubu.game.particle_spawn and kyubu.game.available("particles")) then
    if stage == 0 then
      local _, why = kyubu.game.available("particles")
      kyubu.warn("particle-proto: the particles primitive is not available: " .. tostring(why))
      stage = -1
    end
    return
  end
  if stage == 0 then
    stage, t = 1, 0
    kyubu.info("particle-proto: ready")
  elseif stage == 1 then
    stage, t, next_spawn, count = 2, 0, 0, 0
    kyubu.info("particle-proto: types")
  elseif stage == 2 then
    if t >= next_spawn and count < 24 then
      next_spawn = t + 0.5
      count = count + 1
      local p = info.pos
      local yaw = math.rad(info.yaw)
      -- the row stands 8 blocks in front of the player, across the player's view (right = +x of the view)
      local fx, fy = -math.sin(yaw), math.cos(yaw)
      local rx, ry = fy, -fx
      local colors = { { 1, 0.2, 0.2, 1 }, { 1, 0.6, 0.1, 1 }, { 0.2, 1, 0.2, 1 }, { 0.2, 0.6, 1, 1 }, { 0.8, 0.2, 1, 1 } }
      for ty = 0, 4 do
        local off = (ty - 2) * 3.0
        -- row 1: accel 0 0 10 (the game's hit effect), at ground level
        spawn(p, fx * 8 + rx * off, fy * 8 + ry * off, 0.5,
              { type = ty, count = 20, scale = 0.3, spreading = 4, accel = { 0, 0, 10 }, color = colors[ty + 1] })
        -- row 2: no acceleration, 4 blocks nearer
        spawn(p, fx * 4 + rx * off, fy * 4 + ry * off, 0.5,
              { type = ty, count = 20, scale = 0.3, spreading = 4, accel = { 0, 0, 0 }, color = colors[ty + 1] })
      end
    end
    if t > 14 then
      stage = 3
      kyubu.info("particle-proto: done")
    end
  end
end)
