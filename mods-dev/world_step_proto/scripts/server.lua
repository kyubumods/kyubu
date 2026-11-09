-- world_step_proto, server side (probes world-step-hit-client and world-step-hit-server; add-skills-unleashed-multiplayer 10.2).
-- Single-player Cube.exe and Server.exe run this side. It counts sim.world_step, checks that kyubu.game.hit is refused outside a
-- step, then, from the 500th step, three times 150 steps apart once a player and a monster are there: puts one living monster 3 blocks in front of a player, hits it with
-- kyubu.game.hit (damage 1, a stun of 300, 400 and 500 ms, a knock up of 4) and logs the stun timer read right after. Offsets are
-- lib/combat's (a cuwo offset +0x10): id 0x08, pos 0x10 (int64 x3), yaw 0x30, vel 0x34, hostile 0x60, stun 0x12C, hp 0x16C.
local ffi = require("ffi")
local BLOCK = 65536
local START, EVERY, TRIES = 500, 150, 3

local function u8(a) return ffi.cast("uint8_t*", a)[0] end
local function u32(a) return ffi.cast("uint32_t*", a)[0] end
local function i32(a) return ffi.cast("int32_t*", a)[0] end
local function f32(a) return ffi.cast("float*", a)[0] end
local function i64(a) return tonumber(ffi.cast("int64_t*", a)[0]) end

local function log(text) kyubu.info("world_step_proto: " .. kyubu.side .. ": " .. text) end

-- The creatures of the World's map (World+4): node left 0, parent 4, right 8, isnil 0x0D, creature 0x18.
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

local steps, dt_sum, dt_bad, tries, landed_ok, done_logged = 0, 0, 0, 0, 0, false
local target = nil

local waits = 0
local function try_hit(world)
  local player, monster = nil, nil
  for _, c in ipairs(creatures(world)) do
    if u8(c + 0x60) == 0 and f32(c + 0x16C) > 0 then player = player or c
    elseif u8(c + 0x60) == 1 and f32(c + 0x16C) > 1 and (target == nil or target == c) then monster = monster or c end
  end
  if not player or not monster then           -- not a try: Server.exe has no player until the client joins
    waits = waits + 1
    if waits % 10 == 1 then log(("waiting: no %s yet"):format(player and "monster" or "player")) end
    return
  end
  tries = tries + 1
  target = monster
  local yaw = math.rad(f32(player + 0x30))
  ffi.cast("int64_t*", monster + 0x10)[0] = i64(player + 0x10) + math.floor(-math.sin(yaw) * 3 * BLOCK)
  ffi.cast("int64_t*", monster + 0x18)[0] = i64(player + 0x18) + math.floor(math.cos(yaw) * 3 * BLOCK)
  ffi.cast("int64_t*", monster + 0x20)[0] = i64(player + 0x20)
  local g = kyubu.game
  local ah, th = g.creature_find(i64(player + 0x08)), g.creature_find(i64(monster + 0x08))
  local stun = 200 + 100 * tries
  local hp = f32(monster + 0x16C)
  local landed, why = g.hit(ah, th, { damage = 1, stun_ms = stun, knock = { 0, 0, 4 } })
  local got = i32(monster + 0x12C)
  log(("try %d: hit %d, landed %s (%s), stun %d asked %d, hp %.2f to %.2f, velocity z %.2f"):format(tries, i64(monster + 0x08),
    tostring(landed), tostring(why), got, stun, hp, f32(monster + 0x16C), f32(monster + 0x3C)))
  if landed == true and got == stun then landed_ok = landed_ok + 1 end
end

kyubu.on("lifecycle.register", function()
  if not (kyubu.game and kyubu.game.hit) then return log("no kyubu.game.hit") end
  local id, err = kyubu.on("sim.world_step", function(ev)
    steps = steps + 1
    dt_sum = dt_sum + ev.dt
    if ev.dt <= 0 or ev.dt > 1000 or ev.step_out == 0 or ev.effects == 0 then dt_bad = dt_bad + 1 end
    if steps % 250 == 0 then log(("steps %d, dt total %d ms, bad %d"):format(steps, dt_sum, dt_bad)) end
    if steps >= START and tries < TRIES and (steps - START) % EVERY == 0 then try_hit(ev.world) end   -- every 150 steps until three hits were tried
    if tries == TRIES and not done_logged then
      done_logged = true
      log(("done: %d of %d hits with the exact stun, %d steps, bad %d"):format(landed_ok, TRIES, steps, dt_bad))
    end
  end)
  log("sim.world_step " .. tostring(id or err))
end)

-- outside a step: refused (sim.tick runs after the step on both hosts)
local outside_logged = false
kyubu.on("sim.tick", function()
  if outside_logged or steps == 0 or not target then return end
  outside_logged = true
  local h = kyubu.game.creature_find(i64(target + 0x08))
  local ok, why = kyubu.game.hit(nil, h, { damage = 1 })
  log("outside a step: " .. tostring(ok) .. ", " .. tostring(why))
end)
