-- bow_model_proto, client side (task 8.2b of add-skills-unleashed, probe bow-model).
-- ModelLoader::itemModel(this, item) picks a model for an item from its type, sub_type, material, modifier and rarity (items
-- atlas). A mod model is a slot of the same table (kyubu.model.add, from 2569). The hook gives an item the mod marks (here
-- minus_modifier, +8, set to 0x4C49) the mod's slot through ModelLoader::get; every other item goes on to the game's own choice.
-- For a ranger of specialization 5 the right weapon (equipment 7) is made a marked bow; specialization 6 puts the real one back.
local ffi = require("ffi")

local GC_PLAYER = 0x8006C8
local P_CLASS, P_SPEC = 0x140, 0x141
local SLOT7, ITEM_SIZE = 0xAA8, 0x118
local MARK = 0x4C49

local function u8(a) return ffi.cast("uint8_t*", a)[0] end
local function u32(a) return ffi.cast("uint32_t*", a)[0] end

local model_slot = nil
kyubu.on("lifecycle.register", function()
  local slot, why = kyubu.model.add("bow_model_proto:bow", "su_bow_of_light.cub")
  kyubu.info("bow_model_proto: model add: " .. tostring(slot) .. " " .. tostring(why))
end)
kyubu.on("lifecycle.game_ready", function()
  model_slot = kyubu.model.slot("bow_model_proto:bow")
  kyubu.info("bow_model_proto: model slot " .. tostring(model_slot))
end)

local get_model = nil
local calls, marked = 0, 0
local ok, err = pcall(function()
  kyubu.hook("ModelLoader::itemModel", "void*(__thiscall*)(void*, void*)", function(nxt, this, item)
    calls = calls + 1
    local a = tonumber(ffi.cast("uint32_t", item))
    if a ~= 0 and u8(a) == 3 and ffi.cast("int32_t*", a + 8)[0] == MARK and model_slot then
      get_model = get_model or ffi.cast("void*(__thiscall*)(void*, int32_t)", assert(kyubu.symbol("ModelLoader::get")))
      marked = marked + 1
      if marked == 1 then kyubu.info(("bow_model_proto: a marked item asked for a model (after %d calls): slot %s"):format(calls, tostring(model_slot))) end
      return get_model(this, model_slot)
    end
    return nxt(this, item)
  end)
end)
kyubu.info("bow_model_proto: itemModel hook " .. (ok and "installed" or ("FAILED: " .. tostring(err))))

local real = ffi.new("uint8_t[?]", ITEM_SIZE)
local have, on = false, false
local id, e = kyubu.hook_mid(0x48BFDC, function(ctx)
  local gc = u32(ctx.esp + 0x10)
  local p = gc ~= 0 and u32(gc + GC_PLAYER) or 0
  if p == 0 or u8(p + P_CLASS) ~= 2 then return end
  local spec = u8(p + P_SPEC)
  if spec == 5 and not on then
    if not have then ffi.copy(real, ffi.cast("void*", p + SLOT7), ITEM_SIZE); have = true end
    ffi.cast("uint8_t*", p + SLOT7 + 1)[0] = 6
    ffi.cast("int32_t*", p + SLOT7 + 8)[0] = MARK
    on = true
    kyubu.info("bow_model_proto: the weapon is a marked bow")
  elseif spec == 6 and on then
    ffi.copy(ffi.cast("void*", p + SLOT7), real, ITEM_SIZE)
    on = false
    kyubu.info(("bow_model_proto: the weapon is back; itemModel calls %d, marked %d"):format(calls, marked))
  end
end)
kyubu.info("bow_model_proto: bar site " .. (id and "hooked" or ("FAILED: " .. tostring(e))))
