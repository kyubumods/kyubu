-- su_rank, client side: sets the played character's rank in one skills_unleashed ability to 3 once the character exists (as
-- srefuse does), and, when the option `specialization` names one, makes the character that class and specialization. The options
-- are read each frame, so a replay's `config su_rank ...` console step takes effect at once.
local ffi = require("ffi")

local C_CLASS, C_SPEC = 0x140, 0x141   -- cube::Creature: class_type, specialization (cuwo 0x130 and 0x131, +0x10)
local GC_PLAYER = 0x8006D0             -- cube::GameController.player (lib/sites.lua of skills_unleashed)
local SPECS = {
  berserker = { 1, 0 }, guardian = { 1, 1 }, sniper = { 2, 0 }, scout = { 2, 1 },
  fire_mage = { 3, 0 }, water_mage = { 3, 1 }, assassin = { 4, 0 }, ninja = { 4, 1 },
}

local ranked = nil   -- the ability whose rank is set
local told = nil     -- the specialization last logged

local function option(key, default)
  local c = kyubu.config
  if not c or type(c.get) ~= "function" then return default end
  local ok, v = pcall(c.get, key)
  if ok and v ~= nil then return v end
  return default
end

local function player()
  local g = kyubu.symbol("cube::game_controller")
  if not g then return nil end
  local gc = ffi.cast("uint32_t*", g)[0]
  if gc == 0 then return nil end
  local p = ffi.cast("uint32_t*", gc + GC_PLAYER)[0]
  return p ~= 0 and p or nil
end

kyubu.on("frame.begin", function()
  local spec = option("specialization", "keep")
  local cs = SPECS[spec]
  if cs then
    local p = player()
    if p then
      ffi.cast("uint8_t*", p + C_CLASS)[0] = cs[1]
      ffi.cast("uint8_t*", p + C_SPEC)[0] = cs[2]
      if told ~= spec then
        told = spec
        kyubu.info(("su_rank: the character is class %d, specialization %d (%s)"):format(cs[1], cs[2], spec))
      end
    end
  end
  local id = option("ability", "skills_unleashed:berserker")
  if ranked == id or not kyubu.game or not kyubu.game.ability_rank_set then return end
  local r = kyubu.game.ability_rank_set(id, 3)
  if r then
    ranked = id
    kyubu.info(("su_rank: rank %d set for %s"):format(r, id))
  end
end)
