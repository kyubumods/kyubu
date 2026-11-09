-- anim_catalogue_proto, client side (task 4.5 of add-ability-animation, probe anim-catalogue). One phase per vanilla mode id (FIRST to LAST): the mod writes the local
-- player's current_mode and pins mode_start_time (the ability-force technique, as anim_borrow_proto does), in three steps of FRAMES draws each, at PINS ms. A mid hook
-- at the join site of draw_creature (0x41F135; EDI = the draw record) logs, at the end of each step: the mode the game left in the creature since the last write
-- (a mode that ended or went on by itself shows), the pose id (record +8), the record time (+4) and the smoothed hand vectors (+0x0C, +0x18, +0x24, +0x30), whose change between
-- the steps tells whether the pose follows the mode time. The second step also logs "animcat: shotNNN" (the input takes the screenshot), then holds the pin until
-- SHOT_HOLD s of os.time have passed, so the picture is of that step.
local ffi = require("ffi")

local JOIN = 0x41F135
local GC_PLAYER, P_MODE, P_TIME = 0x8006C8, 0x68, 0x6C
local WARMUP, FRAMES, SHOT_HOLD = 80, 5, 2
local FIRST, LAST = 84, 110  -- a range per run: anim-catalogue used 0 to 110 (the game stopped at 65), anim-catalogue-b 66 to 110 (it stopped at 83), anim-catalogue-c 84 to 110
local PINS = { 100, 450, 800 }

local function rd32(addr)
  local bytes = kyubu.read(addr, 4)
  return bytes and ffi.cast("uint32_t*", bytes)[0] or nil
end
local function player()
  local p = kyubu.symbol("cube::game_controller")
  local gc = p and rd32(p) or 0
  return (gc and gc ~= 0) and (rd32(gc + GC_PLAYER) or 0) or 0
end

local t0, mode, sub, frames, shot_t, finished = nil, FIRST - 1, 0, 0, nil, false
local before
local id_j, err_j = kyubu.hook_mid(JOIN, function(ctx)
  pcall(function()
    local rec = ctx.edi
    local creature = ffi.cast("uint32_t*", rec)[0]
    if creature == 0 or creature ~= player() then return end
    local now = os.time()
    if not t0 then t0 = now; kyubu.info("animcat: first draw of the player") end
    if now - t0 < WARMUP then return end
    if finished then return end
    if sub == 0 then
      mode, sub, frames, shot_t = mode + 1, 1, 0, nil
      if mode > LAST then finished = true; kyubu.info("animcat: done"); return end
      kyubu.info(("animcat: phase mode %d"):format(mode))
    end
    local mode_ptr, time_ptr = ffi.cast("uint8_t*", creature + P_MODE), ffi.cast("uint32_t*", creature + P_TIME)
    before = mode_ptr[0]
    mode_ptr[0] = mode
    time_ptr[0] = mode == 0 and 0 or PINS[sub]
    frames = frames + 1
    if frames == FRAMES then
      local f = ffi.cast("float*", rec)
      local v = {}
      for _, o in ipairs({ 0x0C, 0x18, 0x24, 0x30 }) do
        v[#v + 1] = ("(%.2f %.2f %.2f)"):format(f[o / 4], f[o / 4 + 1], f[o / 4 + 2])
      end
      kyubu.info(("animcat: seen mode %d step %d pin %d game_mode %d pose %d rtime %d hands %s"):format(
        mode, sub, PINS[sub], before, ffi.cast("uint32_t*", rec + 8)[0], ffi.cast("uint32_t*", rec + 4)[0], table.concat(v, " ")))
      if sub == 2 then
        kyubu.info(("animcat: shot%03d mode%d"):format(mode, mode))
        shot_t = now
      end
    end
    if frames >= FRAMES then
      if sub == 2 and now - shot_t < SHOT_HOLD then return end
      if sub == 3 then sub = 0 else sub, frames = sub + 1, 0 end
    end
  end)
end)
kyubu.info(("animcat: mid hook at the join site 0x%X %s, modes %d to %d"):format(JOIN, id_j and "installed" or ("FAILED: " .. tostring(err_j)), FIRST, LAST))
