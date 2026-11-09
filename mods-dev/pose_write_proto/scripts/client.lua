-- pose_write_proto, client side (task 2.5 of add-ability-animation, probe pose-write). draw_creature (0x4129C0) is a __thiscall: ECX is the
-- draw record (Creature + 0x1498). 0x41F135 is the join site found by task 2.4: EDI is the record, and the four adds of the hand vectors
-- (call 0x412920) follow it. For the local player the hook writes one vector of the record on every draw; the phases step by wall clock
-- (os.time), starting WARMUP seconds after the first draw of the player (the script's `wait world` / `wait 20` stall is over by then). The first log line of a phase is "posewrite: phase N label",
-- the second, SHOT_AT seconds later, is "posewrite: shotNN label" (NN two digits) (the replay takes its screenshot on it).
local ffi = require("ffi")

local JOIN = 0x41F135
local GC_PLAYER = 0x8006C8
local WARMUP, PHASE_S, SHOT_AT = 40, 8, 4

-- record offsets of the task 2.3 table: hand A position +0x0C, euler +0x24; hand B position +0x18, euler +0x30
-- (no write to anything of the creature itself: the creature's own fields are not at the record's offsets, see the evidence README, 2.5)
local HA_POS, HA_EUL, HB_POS, HB_EUL = 0x0C, 0x24, 0x18, 0x30
-- a phase is { label, { {offset, axis, value}, ... } }; axis 0 = x, 1 = y, 2 = z, degrees for an euler and model units for a position
local PHASES = {
  { "baseline1", nil },
  { "handB_pos_x_20", { { HB_POS, 0, 20 } } },
  { "baseline2", nil },
  { "handB_pos_y_20", { { HB_POS, 1, 20 } } },
  { "baseline3", nil },
  { "handB_pos_z_20", { { HB_POS, 2, 20 } } },
  { "baseline4", nil },
  { "handB_euler_x_90", { { HB_EUL, 0, 90 } } },
  { "baseline5", nil },
  { "handB_euler_y_90", { { HB_EUL, 1, 90 } } },
  { "baseline6", nil },
  { "handB_euler_z_90", { { HB_EUL, 2, 90 } } },
  { "baseline7", nil },
  { "handB_pos_x_20_euler_z_90", { { HB_POS, 0, 20 }, { HB_EUL, 2, 90 } } },
  { "baseline8", nil },
}

local function rd32(addr)
  local bytes = kyubu.read(addr, 4)
  return bytes and ffi.cast("uint32_t*", bytes)[0] or nil
end
local function player()
  local p = kyubu.symbol("cube::game_controller")
  local gc = p and rd32(p) or 0
  return (gc and gc ~= 0) and (rd32(gc + GC_PLAYER) or 0) or 0
end

-- A phase starts when the previous one has run PHASE_S seconds of draws and its shot line is logged, so a stall of the game (a console command
-- or the inspector takes seconds under software rendering) delays the rest and loses no phase and no shot line.
local t0, phase, phase_t, shot_done, finished = nil, 0, 0, false, false
local id_j, err_j = kyubu.hook_mid(JOIN, function(ctx)
  pcall(function()
    local rec = ctx.edi
    local creature = ffi.cast("uint32_t*", rec)[0]
    if creature == 0 or creature ~= player() then return end
    local now = os.time()
    if not t0 then t0 = now; kyubu.info("posewrite: first draw of the player") end
    if now - t0 < WARMUP then return end
    if phase == 0 or (shot_done and now - phase_t >= PHASE_S and phase <= #PHASES) then
      phase, phase_t, shot_done = phase + 1, now, false
      if phase <= #PHASES then kyubu.info(("posewrite: phase %d %s"):format(phase, PHASES[phase][1])) end
    end
    if phase > #PHASES then
      if not finished then finished = true; kyubu.info("posewrite: done") end
      return
    end
    if not shot_done and now - phase_t >= SHOT_AT then
      shot_done = true
      kyubu.info(("posewrite: shot%02d %s"):format(phase, PHASES[phase][1]))
    end
    for _, w in ipairs(PHASES[phase][2] or {}) do ffi.cast("float*", rec + w[1])[w[2]] = w[3] end
  end)
end)
kyubu.info(("posewrite: mid hook at the join site 0x%X %s"):format(JOIN, id_j and "installed" or ("FAILED: " .. tostring(err_j))))
