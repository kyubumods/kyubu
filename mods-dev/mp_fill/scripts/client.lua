-- mp_fill (task 7.1 of add-ability-api, probe ability-joined): the joined client has no kyubu_probe to write mp, and a level-1 warrior's
-- mp (rage) is 0 until a hit earns it, so the key 4 press would be refused. Every frame the local player's mp is set to 1.
-- Option `hp` (task 8.7 of add-skills-unleashed-multiplayer, replay server-guardian-0720): above 0, the local player's hp is set to it
-- every frame, so a monster cannot kill the player, and each hit is logged with its damage (the game clamps the hp to maxHP every frame).
local ffi = require("ffi")
local GC_PLAYER, P_HP, P_MP = 0x8006D0, 0x16C, 0x170   -- 07-20: GameController.player (0x8006C8 on 07-02); data.hp 0x15C, mp 0x160, + 0x10

local function u32(addr) return ffi.cast("uint32_t*", addr)[0] end

local held = nil      -- the hp written last frame, nil when the option is off
local top = 0         -- the highest hp a frame found since (maxHP once the game has clamped it)

local function option(key, default)
  local c = kyubu.config
  if not c or type(c.get) ~= "function" then return default end
  local ok, v = pcall(c.get, key)
  if ok and type(v) == "number" then return v end
  return default
end

kyubu.on("frame.begin", function()
  local g = kyubu.symbol("cube::game_controller")
  local gc = g and u32(g) or 0
  local p = gc ~= 0 and u32(gc + GC_PLAYER) or 0
  if p == 0 then return end
  ffi.cast("float*", p + P_MP)[0] = 1
  local hold = option("hp", 0)
  if hold > 0 then
    local hp = ffi.cast("float*", p + P_HP)
    local was = hp[0]
    -- the game clamps the hp to maxHP within the frame, so a frame finds maxHP, or maxHP less the damage of the hits since:
    -- `top` is the highest hp found, and a frame below it is logged as a hit of that much
    if held == hold then
      if was > top then top = was
      elseif was < top - 0.001 then kyubu.info(("mp_fill: hp fell by %.2f (from %.2f)"):format(top - was, top)) end
    else
      top = was
    end
    hp[0] = hold
    held = hold
  else
    held, top = nil, 0
  end
end)
