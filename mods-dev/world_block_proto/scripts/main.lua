-- world_block_proto (task 10.1 of add-skills-unleashed-multiplayer): on each side, once the player is in a loaded place, reads 81
-- columns (9 by 9, 4 blocks apart) round a grid point near the player with kyubu.game.world_block, from z 255 down to z -16 (the
-- same blocks on both hosts, whatever height each has for the player), and compares every block with the game's own cube::World::getBlock. It logs the top of each column (the highest solid
-- block, "u" for unknown) and a sum of them, so the client's and the server's lines can be compared. The grid point is the
-- player's position rounded down to 64 blocks, so both hosts pick the same one.
-- Runs in the ability dispatch site (EAX the creature, EDI the World): the game thread, inside the World step.
local ffi = require("ffi")
local bit = require("bit")
local BLOCK = 65536
local GRID, STEP, N, TOP, BOTTOM = 64, 4, 9, 255, -16
local side = kyubu.side
local get_block = nil
local done, tries, last_try = false, 0, -1e9

local function u8(a) return ffi.cast("uint8_t*", a)[0] end
local function i64(a) return tonumber(ffi.cast("int64_t*", a)[0]) end
local function u32(a) return ffi.cast("uint32_t*", a)[0] end

local function solid(t) local k = bit.band(t, 0x1F); return k ~= 0 and k ~= 2 end

-- The game's block type at block x, y, z.
local function game_type(world, x, y, z)
  local b = get_block(ffi.cast("void*", world), nil, x * BLOCK + BLOCK / 2, y * BLOCK + BLOCK / 2, z * BLOCK + BLOCK / 2, 0)
  return b[3]
end

local function survey(world, player)
  local ox = math.floor(i64(player + 0x10) / BLOCK / GRID) * GRID
  local oy = math.floor(i64(player + 0x18) / BLOCK / GRID) * GRID
  local oz = math.floor(i64(player + 0x20) / BLOCK)
  local tops, sum, unknown, blocks, differ = {}, 0, 0, 0, 0
  local first_diff = nil
  for i = 0, N - 1 do
    for j = 0, N - 1 do
      local x, y = ox + (i - 4) * STEP, oy + (j - 4) * STEP
      local top = nil
      local col_unknown = false
      for z = TOP, BOTTOM, -1 do
        local t = kyubu.game.world_block(x, y, z)
        if t == nil then
          col_unknown = true
          break
        end
        blocks = blocks + 1
        local g = game_type(world, x, y, z)
        if g ~= t then
          differ = differ + 1
          first_diff = first_diff or ("%d %d %d world_block %d getBlock %d"):format(x, y, z, t, g)
        end
        if not top and solid(t) then top = z end
      end
      if col_unknown then
        unknown = unknown + 1
        tops[#tops + 1] = "u"
      else
        tops[#tops + 1] = tostring(top or "-")
        sum = sum + (top or 0)
      end
    end
  end
  return ox, oy, oz, tops, sum, unknown, blocks, differ, first_diff
end

kyubu.on("lifecycle.register", function()
  local a = kyubu.symbol("cube::World::getBlock")
  if a then get_block = ffi.cast("const uint8_t*(__fastcall*)(void*, void*, int64_t, int64_t, int64_t, int32_t)", a) end
  kyubu.info(("world_block_proto: %s: terrain %s, getBlock %s"):format(side, tostring(kyubu.game.available("terrain")),
    a and ("0x%X"):format(a) or "missing"))
  local id, err = kyubu.hook_mid("cube::World::abilityDispatchSite", function(ctx)
    if done or not get_block then return end
    local c, world = ctx.eax, ctx.edi
    if c == 0 or world == 0 then return end
    local lp = u32(world + 0xB8)   -- cube::World.local_player: the player on Cube.exe, 0 on Server.exe
    if lp ~= 0 then
      if c ~= lp then return end
    elseif u8(c + 0x60) ~= 0 then   -- Server.exe: a player (hostile type 0)
      return
    end
    local now = os.clock()
    if now - last_try < 1 then return end
    last_try = now
    tries = tries + 1
    local ox, oy, oz, tops, sum, unknown, blocks, differ, first_diff = survey(world, c)
    if unknown > 0 and tries < 90 then
      kyubu.info(("world_block_proto: %s: try %d: %d of 81 columns unknown"):format(side, tries, unknown))
      return
    end
    done = true
    kyubu.info(("world_block_proto: %s: origin %d %d %d: %d columns unknown, %d blocks compared, %d differ%s"):format(side, ox, oy,
      oz, unknown, blocks, differ, first_diff and (" (first " .. first_diff .. ")") or ""))
    kyubu.info(("world_block_proto: %s: tops sum %d: %s"):format(side, sum, table.concat(tops, " ")))
    kyubu.info(("world_block_proto: %s: done"):format(side))
  end)
  kyubu.info("world_block_proto: hook " .. tostring(id or err))
end)
