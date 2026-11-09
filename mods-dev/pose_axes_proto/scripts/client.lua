-- pose_axes_proto, client side (task 2.8 of add-ability-animation, probe pose-axes). Same hook as pose_write_proto: 0x41F135 is the join site of
-- draw_creature (EDI = the draw record, Creature + 0x1498). For the local player the hook writes ONE euler axis of ONE part on every draw (+30 or
-- -30 degrees, absolute), phase after phase: a baseline (no write) first, then the six writes of a part, then the next part. The phases run twice:
-- the front view (the player's own yaw, which a new world starts at 180) and the left profile (the hook writes body_yaw = 90 on every draw of the
-- player; body_yaw is at Creature + 0x30). A phase is PHASE_S seconds long (whole seconds of os.time); its screenshot line "poseaxes: shotNN label"
-- comes SHOT_AT seconds in, and the next phase starts only when that line is out, so a stall of the game loses no shot.
local ffi = require("ffi")

local JOIN = 0x41F135
local GC_PLAYER = 0x8006C8
local WARMUP, PHASE_S, SHOT_AT = 40, 5, 2
local YAW = 0x30 -- body_yaw: Creature + 0x30 (the cuwo offset 0x20 plus the object's 0x10)

-- euler offsets of the task 2.3 table; `slot` is the record's model slot of the part (a float holding the model pointer, 0 = not drawn)
local PARTS = {
  { "root", 0x60, nil }, { "body", 0x54, 0xC4 }, { "head", 0x84, 0xBC },
  { "handA", 0x24, 0xCC }, { "handB", 0x30, 0xCC }, { "tail", 0x90, 0xD4 },
}
local AXES = { "x", "y", "z" }
local VIEWS = { { "front", nil }, { "left", 90 } }

local PHASES = {} -- { label, offset or nil, axis, value, yaw or nil }
for _, v in ipairs(VIEWS) do
  for _, p in ipairs(PARTS) do
    PHASES[#PHASES + 1] = { ("%s_%s_baseline"):format(v[1], p[1]), nil, nil, nil, v[2] }
    for a = 0, 2 do
      for _, sign in ipairs({ 30, -30 }) do
        -- no "+" or "-" in a label: it is also a screenshot's file name
        PHASES[#PHASES + 1] = { ("%s_%s_%s%s30"):format(v[1], p[1], AXES[a + 1], sign > 0 and "p" or "m"), p[2], a, sign, v[2] }
      end
    end
  end
end

local function rd32(addr)
  local bytes = kyubu.read(addr, 4)
  return bytes and ffi.cast("uint32_t*", bytes)[0] or nil
end
local function player()
  local p = kyubu.symbol("cube::game_controller")
  local gc = p and rd32(p) or 0
  return (gc and gc ~= 0) and (rd32(gc + GC_PLAYER) or 0) or 0
end

local t0, phase, phase_t, shot_done, finished, slots_logged = nil, 0, 0, false, false, false
local seen = {} -- other creatures' model-slot masks, logged once per distinct mask
local id_j, err_j = kyubu.hook_mid(JOIN, function(ctx)
  pcall(function()
    local rec = ctx.edi
    local creature = ffi.cast("uint32_t*", rec)[0]
    if creature == 0 then return end
    local mask = 0
    for i, off in ipairs({ 0xBC, 0xC0, 0xC4, 0xC8, 0xCC, 0xD0, 0xD4, 0xD8, 0xDC, 0xE0, 0xE4, 0xE8 }) do
      if ffi.cast("uint32_t*", rec + off)[0] ~= 0 then mask = mask + 2 ^ (i - 1) end
    end
    if creature ~= player() then
      if not seen[mask] then
        seen[mask] = true
        kyubu.info(("poseaxes: other creature 0x%X slot mask 0x%X"):format(creature, mask))
      end
      return
    end
    local now = os.time()
    if not t0 then t0 = now; kyubu.info("poseaxes: first draw of the player") end
    if not slots_logged then
      slots_logged = true
      kyubu.info(("poseaxes: player slot mask 0x%X (bits: head hair body shoulder2 hands wings tail feet weaponA weaponB shoulders neck)"):format(mask))
    end
    if now - t0 < WARMUP then return end
    if phase == 0 or (shot_done and now - phase_t >= PHASE_S and phase <= #PHASES) then
      phase, phase_t, shot_done = phase + 1, now, false
      if phase <= #PHASES then kyubu.info(("poseaxes: phase %d %s"):format(phase, PHASES[phase][1])) end
    end
    if phase > #PHASES then
      if not finished then finished = true; kyubu.info("poseaxes: done") end
      return
    end
    local ph = PHASES[phase]
    if ph[5] then ffi.cast("float*", creature + YAW)[0] = ph[5] end
    if not shot_done and now - phase_t >= SHOT_AT then
      shot_done = true
      kyubu.info(("poseaxes: shot%02d %s"):format(phase, ph[1]))
    end
    if ph[2] then ffi.cast("float*", rec + ph[2])[ph[3]] = ph[4] end
  end)
end)
kyubu.info(("poseaxes: mid hook at the join site 0x%X %s, %d phases"):format(JOIN, id_j and "installed" or ("FAILED: " .. tostring(err_j)), #PHASES))
