-- anim_tracks_proto, client side (task 5.1 of add-ability-animation, probe anim-tracks). Six mod abilities (numbers 112 to 117 in id order):
-- a_plain has no clip, b_rot30, c_arm and d_head have clips with tracks only, e_borrow's clip borrows pose 2 and f_both's borrows it and turns the head.
-- The mod forces the local player's current_mode (the ability-force technique: write current_mode and mode_start_time) and pins the mode time at PIN ms
-- on every draw, so each phase is one frozen moment. A mid hook at the join site of draw_creature (0x41F135) logs the vectors the draw reads there, and
-- frame.end logs them again after the draw: the loader must have put the game's own values back. Phases are PHASE_S seconds of os.time; the screenshot
-- line "animtracks: shotNN label" comes SHOT_AT seconds in, and the next phase starts only when it is out.
local ffi = require("ffi")

local JOIN = 0x41F135
local GC_PLAYER, P_MODE, P_TIME, P_RECORD, P_CLASS = 0x8006C8, 0x68, 0x6C, 0x1498, 0x140
local HAND_R_POS, HAND_R_ROT, HEAD_ROT, TAIL_ROT, TAIL_SLOT = 0x18, 0x30, 0x84, 0x90, 0xD4 -- the draw record's vectors (three floats each); hand_r is the game's hand B; TAIL_SLOT the tail's model pointer (task 5.2)
local WARMUP, PHASE_S, SHOT_AT, PIN = 80, 6, 3, 300
local LOOP = true   -- after the last phase, start again: the five-minute run (task 5.3) keeps tracks drawn; the first cycle is unchanged
local NS = "anim_tracks_proto:"

-- { label, forced mode (nil: none), class to write (nil: none) }; the mod numbers are checked against on_start's ctx.number, logged, and a
-- wrong guess shows in the log. The four t_ phases (task 5.2) run the tail clip for each class: 1 warrior, 2 mage, 3 ranger, 4 rogue.
local PHASES = {
  { "idle", nil },
  { "a_plain", 112 },
  { "b_rot30", 113 },
  { "c_arm", 114 },
  { "d_head", 115 },
  { "e_borrow", 116 },
  { "f_both", 117 },
  { "idle_after", 0 },
  { "t1_warrior", 118, 1 },
  { "t2_mage", 118, 2 },
  { "t3_ranger", 118, 3 },
  { "t4_rogue", 118, 4 },
}
local CLIPS = { b_rot30 = "rot30", c_arm = "arm", d_head = "head", e_borrow = "borrow", f_both = "both", g_tail = "tail" }

kyubu.on("lifecycle.register", function()
  for _, name in ipairs({ "a_plain", "b_rot30", "c_arm", "d_head", "e_borrow", "f_both", "g_tail" }) do
    local def = {
      icon = "cubeworld:smash",
      duration_ms = 800,
      max_rank = 1,
      cooldown_ms = function() return 1000 end,
      on_start = function(ctx) kyubu.info(("animtracks: on_start %s number %d"):format(ctx.id, ctx.number)) end,
    }
    if CLIPS[name] then def.anim = NS .. CLIPS[name] end
    local ok, err = kyubu.ability.define(NS .. name, def)
    kyubu.info(("animtracks: define %s%s: %s"):format(NS, name, ok and "ok" or tostring(err)))
  end
end)

local function rd32(addr)
  local bytes = kyubu.read(addr, 4)
  return bytes and ffi.cast("uint32_t*", bytes)[0] or nil
end
local function player()
  local p = kyubu.symbol("cube::game_controller")
  local gc = p and rd32(p) or 0
  return (gc and gc ~= 0) and (rd32(gc + GC_PLAYER) or 0) or 0
end
local function vectors(rec)
  local p, r, h = ffi.cast("float*", rec + HAND_R_POS), ffi.cast("float*", rec + HAND_R_ROT), ffi.cast("float*", rec + HEAD_ROT)
  return ("hand_r pos %.2f %.2f %.2f rot %.2f %.2f %.2f head rot %.2f %.2f %.2f"):format(p[0], p[1], p[2], r[0], r[1], r[2], h[0], h[1], h[2])
end

local t0, phase, phase_t, shot_done, finished, after_due = nil, 0, 0, false, false, nil
local id_j, err_j = kyubu.hook_mid(JOIN, function(ctx)
  pcall(function()
    local rec = ctx.edi
    local creature = ffi.cast("uint32_t*", rec)[0]
    if creature == 0 or creature ~= player() then return end
    local now = os.time()
    if not t0 then t0 = now; kyubu.info("animtracks: first draw of the player") end
    if now - t0 < WARMUP then return end
    if phase == 0 or (shot_done and now - phase_t >= PHASE_S and phase <= #PHASES) then
      phase, phase_t, shot_done = phase + 1, now, false
      if phase <= #PHASES then kyubu.info(("animtracks: phase %d %s"):format(phase, PHASES[phase][1])) end
    end
    if phase > #PHASES then
      if not finished then finished = true; kyubu.info("animtracks: done") end
      if LOOP then phase = 0 end   -- the next draw starts phase 1 again
      return
    end
    local ph = PHASES[phase]
    local mode_ptr, time_ptr = ffi.cast("uint8_t*", creature + P_MODE), ffi.cast("uint32_t*", creature + P_TIME)
    if ph[2] then
      mode_ptr[0] = ph[2]
      time_ptr[0] = ph[2] == 0 and 0 or PIN
    end
    if ph[3] then ffi.cast("uint8_t*", creature + P_CLASS)[0] = ph[3] end
    if not shot_done and now - phase_t >= SHOT_AT then
      shot_done = true
      local pose, time = ffi.cast("uint32_t*", rec + 8)[0], ffi.cast("uint32_t*", rec + 4)[0]
      kyubu.info(("animtracks: join %s mode %d record pose %d record time %d %s"):format(ph[1], mode_ptr[0], pose, time, vectors(rec)))
      local tr = ffi.cast("float*", rec + TAIL_ROT)
      kyubu.info(("animtracks: lacking %s class %d tail slot 0x%X tail rot %.2f %.2f %.2f"):format(ph[1], ffi.cast("uint8_t*", creature + P_CLASS)[0],
        ffi.cast("uint32_t*", rec + TAIL_SLOT)[0], tr[0], tr[1], tr[2]))
      after_due = ph[1]
      kyubu.info(("animtracks: shot%02d %s"):format(phase, ph[1]))
    end
  end)
end)
kyubu.info(("animtracks: mid hook at the join site 0x%X %s, %d phases"):format(JOIN, id_j and "installed" or ("FAILED: " .. tostring(err_j)), #PHASES))

-- After the frame's draws: the record holds the game's own values again, whatever the tracks wrote for the draw.
kyubu.on("frame.end", function()
  if not after_due then return end
  local label = after_due
  after_due = nil
  pcall(function()
    local creature = player()
    if creature == 0 then return end
    kyubu.info(("animtracks: after %s %s"):format(label, vectors(creature + P_RECORD)))
  end)
end)
