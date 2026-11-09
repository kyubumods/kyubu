-- anim_borrow_proto, client side (task 4.2 of add-ability-animation, probe anim-borrow). Four mod abilities (numbers 112 to 115 in id order):
-- a_plain has no clip, b_borrow's clip borrows pose 2, c_hide's clip borrows it and hides the head and the weapons. The mod forces the local player's
-- current_mode (the ability-force technique: write current_mode and mode_start_time) and pins the mode time at PIN ms on every draw, so each phase is
-- one frozen moment. Phase 2 forces vanilla mode 57, whose pose id is 2: phase 4 (b_borrow) must draw the same picture. A mid hook at the join site of
-- draw_creature (0x41F135, after the detour wrote the record) logs the pose id (+8) and the time (+4) the draw reads. Phases are PHASE_S seconds of
-- os.time; the screenshot line "animborrow: shotNN label" comes SHOT_AT seconds in, and the next phase starts only when it is out.
local ffi = require("ffi")

local JOIN = 0x41F135
local GC_PLAYER, P_MODE, P_TIME = 0x8006C8, 0x68, 0x6C
local WARMUP, PHASE_S, SHOT_AT, PIN = 80, 6, 3, 300
local NS = "anim_borrow_proto:"

-- { label, forced mode (nil: none), pin in ms (nil: PIN) }; the mod numbers are checked against on_start's ctx.number, logged, and a wrong guess shows in the log
local PHASES = {
  { "idle", nil },
  { "vanilla57", 57 },
  { "a_plain", 112 },
  { "b_borrow", 113 },
  { "c_hide", 114 },
  { "idle_after", 0 },
  -- task 4.3: d_strip (115) has duration "ability" (800 ms) and hides the hair from 0.3 to 0.7. The record reads about 110 to 130 ms more than the pin
  -- (the draw's step on top of it, see the record time in the log), so the pins 0, 273 and 620 aim at about 17, 52 and 95 % of the ability. A pin of 673
  -- (100 %) draws the game's pose 0: the ability has ended (800 ms) and the game put the creature back in mode 0, so 95 % is the last borrowed draw.
  { "strip00", 115, 0 },
  { "strip50", 115, 273 },
  { "strip100", 115, 620 },
}

kyubu.on("lifecycle.register", function()
  for i, name in ipairs({ "a_plain", "b_borrow", "c_hide", "d_strip" }) do
    local def = {
      icon = "cubeworld:smash",
      duration_ms = 800,
      max_rank = 1,
      cooldown_ms = function() return 1000 end,
      on_start = function(ctx) kyubu.info(("animborrow: on_start %s number %d"):format(ctx.id, ctx.number)) end,
    }
    if name == "b_borrow" then def.anim = NS .. "borrow" end
    if name == "c_hide" then def.anim = NS .. "borrow_hide" end
    if name == "d_strip" then def.anim = NS .. "borrow_strip" end
    local ok, err = kyubu.ability.define(NS .. name, def)
    kyubu.info(("animborrow: define %s%s: %s"):format(NS, name, ok and "ok" or tostring(err)))
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

local t0, phase, phase_t, shot_done, finished = nil, 0, 0, false, false
local id_j, err_j = kyubu.hook_mid(JOIN, function(ctx)
  pcall(function()
    local rec = ctx.edi
    local creature = ffi.cast("uint32_t*", rec)[0]
    if creature == 0 or creature ~= player() then return end
    local now = os.time()
    if not t0 then t0 = now; kyubu.info("animborrow: first draw of the player") end
    if now - t0 < WARMUP then return end
    if phase == 0 or (shot_done and now - phase_t >= PHASE_S and phase <= #PHASES) then
      phase, phase_t, shot_done = phase + 1, now, false
      if phase <= #PHASES then kyubu.info(("animborrow: phase %d %s"):format(phase, PHASES[phase][1])) end
    end
    if phase > #PHASES then
      if not finished then finished = true; kyubu.info("animborrow: done") end
      return
    end
    local ph = PHASES[phase]
    local mode_ptr, time_ptr = ffi.cast("uint8_t*", creature + P_MODE), ffi.cast("uint32_t*", creature + P_TIME)
    if ph[2] then
      mode_ptr[0] = ph[2]
      time_ptr[0] = ph[2] == 0 and 0 or ph[3] or PIN
    end
    if not shot_done and now - phase_t >= SHOT_AT then
      shot_done = true
      local pose, time = ffi.cast("uint32_t*", rec + 8)[0], ffi.cast("uint32_t*", rec + 4)[0]
      -- the model slots of the record that the hide mask clears (pose_axes_proto's bits: head 0xBC, hair 0xC0, body 0xC4, shoulders 0xE4, weapons 0xDC and 0xE0)
      local slots = 0
      for i, off in ipairs({ 0xBC, 0xC0, 0xC4, 0xE4, 0xDC, 0xE0 }) do
        if ffi.cast("uint32_t*", rec + off)[0] ~= 0 then slots = slots + 2 ^ (i - 1) end
      end
      kyubu.info(("animborrow: seen %s mode %d creature time %d record pose %d record time %d slots 0x%X (head hair body shoulders weaponA weaponB)"):format(ph[1], mode_ptr[0], time_ptr[0], pose, time, slots))
      kyubu.info(("animborrow: shot%02d %s"):format(phase, ph[1]))
    end
  end)
end)
kyubu.info(("animborrow: mid hook at the join site 0x%X %s, %d phases"):format(JOIN, id_j and "installed" or ("FAILED: " .. tostring(err_j)), #PHASES))
