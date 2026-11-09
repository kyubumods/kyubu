-- weapon_swap_proto, client side (task 8.2b of add-skills-unleashed, probe weapon-swap).
-- The player's right weapon is equipment slot 7 (Creature+0xAA8, 0x118 bytes: type, sub_type, ..., material at +0xD). The bar and
-- the attack mode are read from it every frame, and the model is drawn from it, so writing sub_type 6 makes a bow of whatever
-- weapon the player holds. The game saves the player from save_character (every 60 s in a world, on leaving it, on disconnect):
-- it copies the player, equipment included, so the real weapon has to be back while that call runs.
--   class 2, specialization 0: the bow is put in the slot and stays; a save would write it;
--   specialization 1: the copy of the player that a save writes gets the real weapon back; the player keeps the bow;
--   specialization 2: the real weapon is put back for good.
local ffi = require("ffi")

local GC_PLAYER, GC_BAR = 0x8006C8, 0x80080C
local P_MODE, P_CLASS, P_SPEC = 0x68, 0x140, 0x141
local SLOT7, ITEM_SIZE = 0xAA8, 0x118

local function u8(a) return ffi.cast("uint8_t*", a)[0] end
local function u32(a) return ffi.cast("uint32_t*", a)[0] end

local real = ffi.new("uint8_t[?]", ITEM_SIZE)      -- the player's own weapon, as it was
local have_real = false
local bow_in = false

local function controller()
  local p = kyubu.symbol("cube::game_controller")
  return p and u32(p) or 0
end
local function describe(p)
  return ("slot 7: type %d sub_type %d material %d level %d"):format(u8(p + SLOT7), u8(p + SLOT7 + 1), u8(p + SLOT7 + 0xD),
    ffi.cast("int32_t*", p + SLOT7 + 0x10)[0])
end

local function bow_on(p)
  if not have_real then
    ffi.copy(real, ffi.cast("void*", p + SLOT7), ITEM_SIZE)
    have_real = true
  end
  ffi.cast("uint8_t*", p + SLOT7 + 1)[0] = 6
  bow_in = true
end
local function bow_off(p)
  if have_real then ffi.copy(ffi.cast("void*", p + SLOT7), real, ITEM_SIZE) end
  bow_in = false
end

local seen = { spec = -1, bar = "", slot = "" }
local id_bar, e_bar = kyubu.hook_mid(0x48BFDC, function(ctx)
  local gc = u32(ctx.esp + 0x10)
  local p = gc ~= 0 and u32(gc + GC_PLAYER) or 0
  if p == 0 or u8(p + P_CLASS) ~= 2 then return end
  local spec = u8(p + P_SPEC)
  if spec ~= seen.spec then
    kyubu.info(("weapon_swap_proto: specialization %d"):format(spec))
    seen.spec = spec
  end
  if spec == 2 then
    if bow_in then bow_off(p) end
  elseif not bow_in then
    bow_on(p)
  end
  local b, e = u32(gc + GC_BAR), u32(gc + GC_BAR + 4)
  local ids = {}
  for a = b, e - 4, 4 do ids[#ids + 1] = ffi.cast("int32_t*", a)[0] end
  local bar = ("bar [%s]"):format(table.concat(ids, ", "))
  local slot = describe(p)
  if bar ~= seen.bar or slot ~= seen.slot then
    kyubu.info(("weapon_swap_proto: %s, %s, mode %d"):format(slot, bar, u8(p + P_MODE)))
    seen.bar, seen.slot = bar, slot
  end
end)
kyubu.info("weapon_swap_proto: bar site " .. (id_bar and "hooked" or ("FAILED: " .. tostring(e_bar))))

-- The save is not detoured: the 60 s autosave runs save_character on a worker thread, which takes the World's lock for the copy,
-- and a Lua detour that waits for that lock with the Lua state in its hand freezes the game (the main thread holds the lock and
-- waits for the Lua state: seen in play on 2026-10-12). cube::GameController::saveCopiedSite is inside save_character, after the
-- copy and the World's unlock: EBX is the copy that is about to be written, [EBP+0xC] the player it was made of.
ffi.cdef("unsigned long GetCurrentThreadId(void);")
local frame_thread = nil
kyubu.hook_mid(0x48BFDC, function() frame_thread = frame_thread or ffi.C.GetCurrentThreadId() end)
local id_save, e_save = kyubu.hook_mid("cube::GameController::saveCopiedSite", function(ctx)
  local copy, p = ctx.ebx, u32(ctx.ebp + 0x0C)
  if copy == 0 or p == 0 then return kyubu.info("weapon_swap_proto: save site: no copy or no player") end
  local guard = u8(p + P_CLASS) == 2 and u8(p + P_SPEC) == 1 and bow_in
  kyubu.info(("weapon_swap_proto: save site (thread %d, the frame's is %s): the copy's %s, guard %s"):format(
    ffi.C.GetCurrentThreadId(), tostring(frame_thread), describe(copy), tostring(guard)))
  if guard then
    ffi.cast("uint8_t*", copy + SLOT7 + 1)[0] = u8(ffi.cast("uint8_t*", real) + 1)
    kyubu.info(("weapon_swap_proto: save site: the copy now has %s; the player keeps %s"):format(describe(copy), describe(p)))
  end
end)
kyubu.info("weapon_swap_proto: save site " .. (id_save and "hooked" or ("FAILED: " .. tostring(e_save))))
