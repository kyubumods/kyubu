-- root_proto, server side (probe creature-root-server; add-skills-unleashed-multiplayer 10.3, design D12).
-- On Server.exe, once a player and a monster are there: puts the monster 4 blocks in front of the player (at its own height: the
-- server's copy of a joined player's z is not the client's, and a creature put inside the terrain is lifted out by the game) with
-- kyubu.game.creature_state_set, roots it (creature_set_flags {rooted = true}), and for 250 World steps knocks it every 25 steps with
-- kyubu.game.hit (knock 15 east, 5 up) and creature_state_set (knock 10, 10, 0). Every step it reads the creature back with
-- creature_state_get: the most it drifted from where it was rooted (x, y), the most it rose, the highest mode. Then it clears the
-- flag and knocks it once more, and logs how far it moved in 50 steps (the control: an unrooted monster is knocked away).
-- The creature list is the World's map (World+4), as world_step_proto walks it; id 0x08, hostile type 0x60, hp 0x16C on a Creature*.
local ffi = require("ffi")
local BLOCK = 65536
local START, ROOTED, KNOCK_EVERY, FREE = 400, 250, 25, 150

local function u8(a) return ffi.cast("uint8_t*", a)[0] end
local function u32(a) return ffi.cast("uint32_t*", a)[0] end
local function f32(a) return ffi.cast("float*", a)[0] end
local function i64(a) return tonumber(ffi.cast("int64_t*", a)[0]) end

local function log(text) kyubu.info("root_proto: " .. kyubu.side .. ": " .. text) end

local function creatures(world)
  local out = {}
  local head = u32(world + 4)
  if head == 0 then return out end
  local todo = { u32(head + 4) }
  while #todo > 0 and #out < 20000 do
    local n = table.remove(todo)
    if n ~= 0 and u8(n + 0x0D) == 0 then
      local c = u32(n + 0x18)
      if c ~= 0 then out[#out + 1] = c end
      todo[#todo + 1] = u32(n + 0)
      todo[#todo + 1] = u32(n + 8)
    end
  end
  return out
end

local g = nil
local steps, phase, t, waits = 0, "wait", 0, 0
local mob, player = nil, nil
local home = nil                          -- where it was rooted (units)
local drift, rise, mode_max, hits_landed = 0, 0, 0, 0
local free_from = nil

local function xy_drift(s, from)
  local dx, dy = (s.pos.x - from.x) / BLOCK, (s.pos.y - from.y) / BLOCK
  return math.sqrt(dx * dx + dy * dy)
end

local function start(world)
  local p, m = nil, nil
  for _, c in ipairs(creatures(world)) do
    if u8(c + 0x60) == 0 and f32(c + 0x16C) > 0 then p = p or c
    elseif u8(c + 0x60) == 1 and f32(c + 0x16C) > 1 then m = m or c end
  end
  if not p or not m then
    waits = waits + 1
    if waits % 10 == 1 then log(("waiting: no %s yet"):format(p and "monster" or "player")) end
    return
  end
  player, mob = g.creature_find(i64(p + 0x08)), g.creature_find(i64(m + 0x08))
  local ps = g.creature_state_get(player)
  local own = g.creature_state_get(mob)
  local yaw = math.rad(f32(p + 0x30))
  local to = { x = ps.pos.x + math.floor(-math.sin(yaw) * 4 * BLOCK), y = ps.pos.y + math.floor(math.cos(yaw) * 4 * BLOCK), z = own.pos.z }
  local ok, why = g.creature_state_set(mob, { pos = to, velocity = { x = 0, y = 0, z = 0 }, knock = { x = 0, y = 0, z = 0 } })
  log(("monster %d put at %d %d %d: %s %s"):format(i64(m + 0x08), to.x, to.y, to.z, tostring(ok), tostring(why)))
  local fok, fwhy = g.creature_set_flags(mob, { rooted = true })
  local info = mob:get()
  log(("rooted: %s %s, flags.rooted %s"):format(tostring(fok), tostring(fwhy), tostring(info and info.flags.rooted)))
  local wrong, wwhy = g.creature_set_flags(mob, { rooted = true, unstoppable = true })
  log(("rooted + unstoppable on Server.exe: %s %s"):format(tostring(wrong), tostring(wwhy)))
  phase, t = "rooted", 0
end

local function rooted_step()
  t = t + 1
  local s = g.creature_state_get(mob)
  if not s then phase = "gone"; return log("the monster is gone") end
  if t == 1 then
    home = { x = s.pos.x, y = s.pos.y, z = s.pos.z }
    log(("state: mode %d, mode_ms %d, stun_ms %d, slowed_ms %d, charge %.2f, aim %.2f %.2f %.2f"):format(s.mode, s.mode_ms,
      s.stun_ms, s.slowed_ms, s.charge, s.aim.x, s.aim.y, s.aim.z))
  end
  drift = math.max(drift, xy_drift(s, home))
  rise = math.max(rise, (s.pos.z - home.z) / BLOCK)
  mode_max = math.max(mode_max, s.mode)
  if t % 50 == 0 then log(("rooted step %d: z %.2f blocks, velocity z %.2f"):format(t, s.pos.z / BLOCK, s.velocity.z)) end
  if t % KNOCK_EVERY == 0 then
    local landed = g.hit(player, mob, { damage = 1, stun_ms = 0, knock = { 15, 0, 5 } })
    if landed then hits_landed = hits_landed + 1 end
    g.creature_state_set(mob, { knock = { x = 10, y = 10, z = 0 } })
  end
  if t == ROOTED then
    log(("rooted held: drift %.3f blocks, rise %.3f, mode max %d, %d of %d knocks landed, %d steps"):format(drift, rise, mode_max,
      hits_landed, ROOTED / KNOCK_EVERY, ROOTED))
    local ok, why = g.creature_set_flags(mob, {})
    free_from = { x = s.pos.x, y = s.pos.y }
    local landed = g.hit(player, mob, { damage = 1, stun_ms = 0, knock = { 15, 0, 5 } })
    log(("cleared: %s %s, free knock landed %s"):format(tostring(ok), tostring(why), tostring(landed)))
    phase, t = "free", 0
  end
end

local function free_step()
  t = t + 1
  local s = g.creature_state_get(mob)
  if t % 50 == 0 and s then log(("free step %d: moved %.3f blocks"):format(t, xy_drift(s, free_from))) end
  if t < FREE then return end
  local moved = s and xy_drift(s, free_from) or -1
  log(("free moved %.3f blocks in %d steps"):format(moved, FREE))
  log(("done: rooted drift %.3f, rise %.3f, mode max %d; free moved %.3f"):format(drift, rise, mode_max, moved))
  phase = "done"
end

kyubu.on("lifecycle.register", function()
  g = kyubu.game
  if not (g and g.creature_state_get and g.hit) then return log("no creature_state_get or hit") end
  local id, err = kyubu.on("sim.world_step", function(ev)
    steps = steps + 1
    if phase == "wait" then
      if steps >= START and steps % 25 == 0 then start(ev.world) end
    elseif phase == "rooted" then rooted_step()
    elseif phase == "free" then free_step() end
  end)
  log("sim.world_step " .. tostring(id or err))
end)
