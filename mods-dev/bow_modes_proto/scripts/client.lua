-- bow_modes_proto, client side (task 8.2a of add-skills-unleashed, probe bow-modes). The hook at 0x48BFDC (cube::GameController::
-- abilityBarBuiltSite) runs once per frame inside cube::GameController::update, after the key bytes at GameController+4.. were
-- written by WinMain and before the input loop at 0x49B7E7 reads them. A ranger of specialization 1 gets the M1 byte (+4)
-- copied onto the M2 byte (+5): the loop then sees M2 held, runs the id on bar index 1 and skips the primary attack (the
-- "a slot of 1 or higher was held" flag at ESP+0x17). Everything else is logged when it changes.
local ffi = require("ffi")

local GC_BAR, GC_PLAYER = 0x80080C, 0x8006C8
local P_MODE, P_TIME, P_MP, P_CLASS, P_SPEC = 0x68, 0x6C, 0x170, 0x140, 0x141
local P_CHARGED, P_SPENT = 0x144, 0x13B4
local BRIDGE_CLASS, BRIDGE_SPEC = 2, 1

local function u32(addr) return ffi.cast("uint32_t*", addr)[0] end

local seen = { mode = -1, bar = "", keys = "" }
local bridged = 0

local hook_id, err = kyubu.hook_mid(0x48BFDC, function(ctx)
  local gc = u32(ctx.esp + 0x10)
  if gc == 0 then return end
  local p = u32(gc + GC_PLAYER)
  if p == 0 then return end
  local class = ffi.cast("uint8_t*", p + P_CLASS)[0]
  local spec = ffi.cast("uint8_t*", p + P_SPEC)[0]
  local keys = ffi.cast("uint8_t*", gc)
  if class == BRIDGE_CLASS and spec == BRIDGE_SPEC and keys[4] ~= 0 and keys[5] == 0 then
    keys[5] = 1
    bridged = bridged + 1
    if bridged == 1 then kyubu.info("bow_modes_proto: M1 byte copied onto M2 (first time)") end
  end
  local mode = ffi.cast("uint8_t*", p + P_MODE)[0]
  if mode ~= seen.mode then
    kyubu.info(("bow_modes_proto: mode %d -> %d (mode time %d ms, mp %.3f, charged %.3f, spent %.3f, keys M1 %d M2 %d)"):format(
      seen.mode, mode, tonumber(u32(p + P_TIME)), ffi.cast("float*", p + P_MP)[0], ffi.cast("float*", p + P_CHARGED)[0],
      ffi.cast("float*", p + P_SPENT)[0], keys[4], keys[5]))
    seen.mode = mode
  end
  local b, e = u32(gc + GC_BAR), u32(gc + GC_BAR + 4)
  local ids = {}
  for a = b, e - 4, 4 do ids[#ids + 1] = ffi.cast("int32_t*", a)[0] end
  local bar = ("class %d spec %d bar [%s]"):format(class, spec, table.concat(ids, ", "))
  if bar ~= seen.bar then kyubu.info("bow_modes_proto: " .. bar); seen.bar = bar end
end)
kyubu.info(("bow_modes_proto: mid hook at 0x48BFDC %s"):format(hook_id and "installed" or ("FAILED: " .. tostring(err))))

-- The two arrow sites: just before the game pushes the new cube::Projectile (the push is a `call 0x486670`, a relative call that
-- the hook engine refuses to displace). The projectile is complete on the stack, at EBP-0x578 in the block of modes 22, 23 and 26
-- and at EBP-0xDE0 in the block of 50, 55 and 96; its damage is still in XMM1. Logged field by field: kind (+0x60), skill
-- (+0x5C), heals (+0x64), velocity (+0x3C), scale (+0x50), mana (+0x54), age (+0x68).
local function log_projectile(tag, ctx, below)
  local gc_ptr = kyubu.symbol("cube::game_controller")
  local gc = gc_ptr and u32(gc_ptr) or 0
  local p = gc ~= 0 and u32(gc + GC_PLAYER) or 0
  local proj = ctx.ebp - below
  local f = function(o) return ffi.cast("float*", proj + o)[0] end
  kyubu.info(("bow_modes_proto: %s: shooter %d kind %d skill %d heals %d damage %.3f vel (%.2f, %.2f, %.2f) scale %.3f mana %.3f age %d, mode %d, class %d weapon %d/%d"):format(
    tag, tonumber(ffi.cast("int64_t*", proj)[0]), ffi.cast("int32_t*", proj + 0x60)[0], ffi.cast("uint8_t*", proj + 0x5C)[0],
    ffi.cast("uint8_t*", proj + 0x64)[0], ctx.xmm[1].f32[0], f(0x3C), f(0x40), f(0x44), f(0x50), f(0x54), ffi.cast("int32_t*", proj + 0x68)[0],
    p ~= 0 and ffi.cast("uint8_t*", p + P_MODE)[0] or -1, p ~= 0 and ffi.cast("uint8_t*", p + P_CLASS)[0] or -1,
    p ~= 0 and ffi.cast("uint8_t*", p + 0xAA8)[0] or -1, p ~= 0 and ffi.cast("uint8_t*", p + 0xAA9)[0] or -1))
end
for _, site in ipairs({ { 0x615E03, 0x578, "arrow of modes 22, 23, 26" }, { 0x6162D6, 0xDE0, "arrow of modes 50, 55, 96" } }) do
  local id, e = kyubu.hook_mid(site[1], function(ctx) pcall(log_projectile, site[3], ctx, site[2]) end)
  kyubu.info(("bow_modes_proto: mid hook at 0x%X %s"):format(site[1], id and "installed" or ("FAILED: " .. tostring(e))))
end
