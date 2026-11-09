-- velwrite_probe, client side (add-skills-unleashed, probe ability-walk-speed).
-- The walk velocity (Creature + 0x34, + 0x38) peaks 0.72 times lower in mode 112 (a mod ability's mode) than on foot, with the
-- same acceleration. What earlier runs found:
--   runs 7 to 11: no float or dword store to [reg + 0x34] or [reg + 0x24] (709 sites) writes the player's velocity during the walk;
--   run 12: the velocity changes only inside the 20 ms cube::World::update steps (0x48D170 loop), never in the input code;
--   run 13: the movement rate of the step ([ebp-0x2B58]: 6.0, doubled at 0x618653 while bit 0x40 of data.flags is set; the
--   player has the bit on foot, not in mode 112) is not it: with the bit forced in mode 112 the walk peaked at the same 7.109.
--   It is a cap (0x619304: the speed is cut to it), and neither walk reaches it.
--   run 14 (90 points, every 0x400 bytes of cube::World::update): two spans change it, 0x6147C9 to 0x619868 speeds it up
--   (mean ratio a step 1.060 on foot, 1.046 in mode 112) and 0x61B8A0 to 0x61C0A6 slows it (drag, about 0.95 in both).
--   0x5A4B90, called at 0x6195FE and added to the velocity, is a noise field of the position (wind), not the walk.
--   run 15 (every 0x40 bytes of the step): the cap's write-back (0x619352 to 0x619392) cuts the speed, a mean 0.924 a step on
--   foot and 0.696 in mode 112; in mode 112 the step also passes 0x61923E, the lock-on block. While bit 4 of data.flags is set
--   (lock-on, 4 in mode 112), it compares the facing (Creature + 0x160) with the velocity; when they point apart it multiplies
--   the cap by 0.5 * cos + 1 (0x6192AF to 0x6192CF): 0.5 walking straight away from the facing.
--   run 16: with that factor made 1.0 in mode 112 nothing changed (7.06): the site was never reached, the walk does not point
--   away from the facing in this replay.
--   run 17: the cap is lengthSq-compared (178.5 > 144 on foot: the cap 12 binds); in phase B it is 6 (bit 0x40 clear), at times
--   3 (x0.5 at 0x6191C5 for the timer at Creature + 0x134). The bit is the player's "full speed": after the input each frame,
--   cube::GameController sets it when GameController + 0x14 is 0 (0x498500), then, unless the mode is on a list, while the mode
--   time is below cube::Creature::modeDuration + 200 ms it sets bit 4 (lock-on) and clears bit 0x40 (0x4985C6 to 0x4985DF).
--   run 18: bit 0x40 set again at 0x4985E4 in mode 112: the walk peaked at 9.55 (9.85 on foot, 7.11 before). That is the slow.
-- The fix (skills_unleashed lib/sites on_walk, used by the Water Mage) hooks the mapped sites by name. This version does what it
-- does, by the same names, for the player in mode 112: the bit the input gave is kept at cube::GameController::walkLockSite and
-- put back at walkLockEndSite, and the lock-on factor at cube::World::lockOnWalkPenaltySite is made 1.
local ffi = require("ffi")

local GC_PLAYER, P_MODE, P_FLAGS = 0x8006C8, 0x68, 0x124
local MOD_MODE = 112

local function u8(a) return ffi.cast("uint8_t*", a)[0] end
local function u32(a) return ffi.cast("uint32_t*", a)[0] end
local function f32(a) return ffi.cast("float*", a)[0] end
local function flags(c) return ffi.cast("uint16_t*", c + P_FLAGS) end

local keep, player = 0, 0
local kept, cancelled = 0, 0
local function hook(name, fn)
  local id, err = kyubu.hook_mid(name, fn)
  kyubu.info(("velprobe: %s %s"):format(name, id and "hooked" or ("FAILED: " .. tostring(err))))
end
hook("cube::GameController::walkLockEndSite", function(ctx)
  if keep ~= 0 and keep == u32(ctx.edi + GC_PLAYER) then
    flags(keep)[0] = bit.bor(flags(keep)[0], 0x40)
    kept = kept + 1
  end
  keep = 0
end)
hook("cube::GameController::walkLockSite", function(ctx)
  local p = u32(ctx.edi + GC_PLAYER)
  player = p
  if p ~= 0 and u8(p + P_MODE) == MOD_MODE and bit.band(flags(p)[0], 0x40) ~= 0 then keep = p end
end)
hook("cube::World::lockOnWalkPenaltySite", function(ctx)
  local c = u32(ctx.ebp - 0x2B14)
  if c ~= 0 and c == player and u8(c + P_MODE) == MOD_MODE then
    ctx.xmm[0].f32[0] = 1.0
    cancelled = cancelled + 1
  end
end)

local frames = 0
kyubu.on("frame.begin", function()
  frames = frames + 1
  if frames % 120 ~= 0 or (kept == 0 and cancelled == 0) then return end
  local p = kyubu.symbol("cube::game_controller")
  local c = p and u32(u32(p) + GC_PLAYER) or 0
  if c == 0 then return end
  kyubu.info(("velprobe @%d mode %d flags %d vel (%.3f, %.3f): bit 0x40 kept %d times, lock-on factor cancelled %d"):format(frames,
    u8(c + P_MODE), flags(c)[0], f32(c + 0x34), f32(c + 0x38), kept, cancelled))
  kept, cancelled = 0, 0
end)
