-- su_dummy, server side: a target for the server's slot-4 ability. Hooked at the ability dispatch site (once per creature per step,
-- EAX the creature, EDI the World). Offsets are those of lib/combat (a cuwo offset +0x10): id 0x08, pos 0x10 (int64 x3), yaw 0x30,
-- vel 0x34, hostile 0x60, mode 0x68, hp 0x16C.
local ffi = require("ffi")
local BLOCK = 65536
local player, target = nil, nil
local last_hp, last_log = nil, 0
local struck = false   -- the target lost hp: it is no longer held in place, so the strike's throw can carry it

local function u8(a) return ffi.cast("uint8_t*", a)[0] end
local function f32(a) return ffi.cast("float*", a)[0] end

kyubu.on("lifecycle.register", function()
  local id, err = kyubu.hook_mid("cube::World::abilityDispatchSite", function(ctx)
    local c = ctx.eax
    if c == 0 then return end
    local mode = u8(c + 0x68)
    if u8(c + 0x60) == 0 and mode >= 111 then
      player = c
    elseif not struck and player and u8(c + 0x60) == 1 and f32(c + 0x16C) > 0 and (target == nil or target == c) then
      if u8(player + 0x68) < 111 then return end
      target = c
      local yaw = math.rad(f32(player + 0x30))
      local px = tonumber(ffi.cast("int64_t*", player + 0x10)[0])
      local py = tonumber(ffi.cast("int64_t*", player + 0x18)[0])
      local pz = tonumber(ffi.cast("int64_t*", player + 0x20)[0])
      ffi.cast("int64_t*", c + 0x10)[0] = px + math.floor(-math.sin(yaw) * 3 * BLOCK)
      ffi.cast("int64_t*", c + 0x18)[0] = py + math.floor(math.cos(yaw) * 3 * BLOCK)
      ffi.cast("int64_t*", c + 0x20)[0] = pz
      ffi.cast("float*", c + 0x34)[0] = 0; ffi.cast("float*", c + 0x38)[0] = 0
    end
    if target == c then
      local hp = f32(c + 0x16C)
      if hp ~= last_hp then
        if last_hp and hp < last_hp then struck = true end
        last_hp = hp
        kyubu.info(("su_dummy: target hp %.2f, velocity z %.2f"):format(hp, f32(c + 0x3C)))
      end
    end
  end)
  kyubu.info("su_dummy: hook " .. tostring(id or err))
end)
