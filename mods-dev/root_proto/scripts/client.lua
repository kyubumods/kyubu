-- root_proto, client side (probe creature-root-server): on the Cube.exe joined to the server, watches the hostile creature nearest
-- the local player (within 6 blocks) at every World step of the client's copy, with kyubu.game.creature_state_get. It logs "held" with
-- the creature's id once the same creature stayed within 0.25 block (x, y) for 150 steps, and "moved" once that creature then went
-- more than 1 block from there (one line each per creature: compare the id with the server's "put at" line).
-- In single-player this side does nothing (the server side does the test there).
local ffi = require("ffi")
local BLOCK = 65536
local HOLD, NEAR = 150, 6

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
local watch, since, anchor, steps = nil, 0, nil, 0
local held = {}                            -- id -> { anchor, moved }

local function dist(a, b)
  local dx, dy = (a.x - b.x) / BLOCK, (a.y - b.y) / BLOCK
  return math.sqrt(dx * dx + dy * dy)
end

local function step(world)
  steps = steps + 1
  local me = g.creature_player()
  local ms = me and g.creature_state_get(me)
  if not ms then return end
  local best, best_d = nil, NEAR
  for _, c in ipairs(creatures(world)) do
    if u8(c + 0x60) == 1 and f32(c + 0x16C) > 0 then
      local h = g.creature_find(i64(c + 0x08))
      local s = h and g.creature_state_get(h)
      if s then
        local d = dist(s.pos, ms.pos)
        if d < best_d then best, best_d = { id = i64(c + 0x08), pos = s.pos }, d end
      end
    end
  end
  for id, e in pairs(held) do
    if not e.moved then
      local h = g.creature_find(id)
      local s = h and g.creature_state_get(h)
      if s and dist(s.pos, e.anchor) > 1 then
        e.moved = true
        log(("moved: monster %d went %.3f blocks from where it was held"):format(id, dist(s.pos, e.anchor)))
      end
    end
  end
  if not best or held[best.id] then watch = nil; return end
  if best.id ~= watch or dist(best.pos, anchor) > 0.25 then
    watch, since, anchor = best.id, 0, best.pos
    return
  end
  since = since + 1
  if since == HOLD then
    held[watch] = { anchor = anchor, moved = false }
    log(("held: monster %d stayed within 0.25 block for %d steps, %.2f blocks from the player"):format(watch, HOLD, best_d))
  end
end

kyubu.on("lifecycle.register", function()
  g = kyubu.game
  if not (g and g.creature_state_get and g.creature_player) then return log("no creature_state_get") end
  kyubu.on("sim.world_step", function(ev)
    local ok, err = pcall(step, ev.world)
    if not ok then log("error: " .. tostring(err)) end
  end)
  log("watching")
end)
