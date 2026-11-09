-- skill_ranks_proto, client side (task 8.2 of add-ability-api, probe ability-learn). The menu-to-world switch zeroes the player's
-- 11 ranks (Creature+0x1138), so while the skill panel is closed (GameController+0x8008E9 is 0) the mod writes rank 5 into
-- skills 6, 7 and 8 whenever one differs: button 9's prerequisite is skill 8 at rank 5. It logs skills[9] and the panel's flag
-- when either changes. It never touches button 9, the skill-icon map or skills[9]: the loader's skill-tree hook does that.
local ffi = require("ffi")

local GC_PANEL_OPEN, GC_PLAYER, P_RANKS = 0x8008E9, 0x8006C8, 0x1138

local function u32(addr) return ffi.cast("uint32_t*", addr)[0] end
local function rd32(addr)
  local bytes = kyubu.read(addr, 4)
  return bytes and ffi.cast("uint32_t*", bytes)[0] or nil
end
local function wr32(addr, value) return kyubu.write(addr, ffi.string(ffi.new("uint32_t[1]", value), 4)) end
local function game_controller()
  local p = kyubu.symbol("cube::game_controller")
  if not p then return 0 end
  return u32(p)
end

local last = ""
kyubu.on("frame.begin", function()
  local gc = game_controller()
  local p = gc ~= 0 and u32(gc + GC_PLAYER) or 0
  if p == 0 then return end
  local open = (kyubu.read(gc + GC_PANEL_OPEN, 1) or " "):byte(1)
  if open == 0 then
    for i = 6, 8 do if rd32(p + P_RANKS + 4 * i) ~= 5 then wr32(p + P_RANKS + 4 * i, 5) end end
  end
  local state = ("panel %d; skills[9] %s"):format(open, tostring(rd32(p + P_RANKS + 36)))
  if state ~= last then last = state; kyubu.info("skill_ranks_proto: " .. state) end
end)

kyubu.info("skill_ranks_proto: loaded")
