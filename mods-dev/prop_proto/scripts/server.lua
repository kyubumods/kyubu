-- prop_proto, server side (task 8.2d of add-skills-unleashed, probe world-model-draw).
-- A creature is the cheapest "model drawn at a point of the mod's choice": the game draws a creature's weapon slots whatever its
-- body parts are, and a part whose model id is -1 draws nothing. So: spawn a still creature, write -1 on its eight appearance
-- model ids (Creature+0x8C..0x9A, int16) and a bow on its right weapon slot (equipment 7, +0xAA8), and move it each tick.
local ffi = require("ffi")
local cw = require("cubeworld")

local BLOCK = 65536
local SLIME = 37

local function fn(name)
  local addr, why = kyubu.symbol(name)
  if not addr then error(("symbol %s: %s"):format(name, tostring(why)), 0) end
  return ffi.cast(cw.functions[name], addr)
end

local function creature_by_id(id)
  local gc = ffi.cast("uintptr_t*", assert(kyubu.symbol("cube::game_controller")))[0]
  local map = ffi.cast("void*", gc + 0x2E8)
  local key = ffi.new("uint64_t[1]", id)
  local slot = fn("creature_map_slot")(map, nil, key)
  return slot[0] ~= nil and slot[0] or nil
end

local props = {}          -- { c = creature object, base = address, dx, x, y, z, ticks, yaw }

-- Two props, to compare what hides the creature's own marks. The first is a friendly creature (hostile type 2) that comes down
-- on the point the player's crosshair is on (`at`), so that the game names it; the client side is told its address and takes its
-- name away (specialization 5). The second is a target (6), three blocks to the player's right. Both are shrunk through the
-- scale vector at Creature+0x80.
local function make_prop(m, dx, hostile, scale, at, conn)
  local px, py, pz = m.x + dx * BLOCK, m.y + 4 * BLOCK, m.z + 18 * BLOCK
  if at then px, py, pz = at.x, at.y, at.z + 16.5 * BLOCK end       -- it comes down 16 blocks: to half a block over the point
  local creature, why = kyubu.game.creature_spawn({
    type = SLIME, hostile = hostile, level = 1, pos = { x = px, y = py, z = pz }, yaw = 0 })
  if not creature then return kyubu.warn("prop_proto: not spawned: " .. tostring(why)) end
  local info = creature:get()
  local raw = creature_by_id(info.id)
  if not raw then return kyubu.warn("prop_proto: the creature is not in the map") end
  local base = tonumber(ffi.cast("uintptr_t", raw))
  for off = 0x8C, 0x9A, 2 do ffi.cast("int16_t*", base + off)[0] = -1 end
  ffi.fill(ffi.cast("void*", base + 0xAA8), 0x118)
  ffi.cast("uint8_t*", base + 0xAA8)[0] = 3            -- type: a weapon
  ffi.cast("uint8_t*", base + 0xAA9)[0] = 6            -- sub_type: a bow
  ffi.cast("int32_t*", base + 0xAA8 + 0x10)[0] = 1     -- level
  local sv = ffi.cast("float*", base + 0x80)
  kyubu.info(("prop_proto: prop spawned: id %d, hostile %d, scale was (%.2f, %.2f, %.2f)"):format(info.id, hostile, sv[0], sv[1], sv[2]))
  sv[0], sv[1], sv[2] = scale, scale, scale
  props[#props + 1] = { c = creature, base = base, x = px, y = py, z = pz, ticks = 0, yaw = math.random(0, 359) }
  if at then kyubu.channel.send("prop_proto:ready", conn, { base = base, id = info.id }) end
end

kyubu.channel.register("prop_proto:spawn", function(conn, m)
  if #props > 0 or type(m) ~= "table" then return end
  make_prop(m, -3, 2, 0.3, m.ax and { x = m.ax, y = m.ay, z = m.az } or nil, conn)
  make_prop(m, 3, 6, 0.3)
end)

kyubu.on("sim.tick", function()
  for i, prop in ipairs(props) do
    if prop.c:valid() then
      prop.ticks = prop.ticks + 1
      local base = prop.base
      local target_z = prop.z - math.min(prop.ticks, 20) * 0.8 * BLOCK
      local pos = ffi.cast("int64_t*", base + 0x10)
      pos[0], pos[1], pos[2] = prop.x, prop.y, target_z
      local vel = ffi.cast("float*", base + 0x34)
      vel[0], vel[1], vel[2] = 0, 0, 0
      prop.yaw = (prop.yaw + 20) % 360
      ffi.cast("float*", base + 0x30)[0] = prop.yaw
      if prop.ticks % 10 == 1 then
        kyubu.info(("prop_proto: prop %d tick %d: z %.1f blocks, yaw %d"):format(i, prop.ticks, tonumber(target_z) / BLOCK, prop.yaw))
      end
    end
  end
end)
