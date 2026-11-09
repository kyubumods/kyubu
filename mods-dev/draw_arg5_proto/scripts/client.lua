-- draw_arg5_proto, client side (task 2.6 of add-ability-animation, probe draw-arg5). draw_creature (0x4129C0) takes argument 5 from
-- GameController + 0x8006E0, which GameController::update (0x489130) sets from GameController + 0x2A4 near its end (static read). The
-- question: a running counter, or the frame time? Every frame.begin stores the frame number and dt (ms since the previous frame); the first
-- draw of the local player after it logs argument 5 with those. A draw of another creature with a different argument 5 in the same frame
-- is logged too. The hook only reads. The script `watch` the argument over a few seconds of idle, then of walking.
local ffi = require("ffi")

local GC_PLAYER, SRC, DST = 0x8006C8, 0x2A4, 0x8006E0
local function u32(addr) return ffi.cast("uint32_t*", addr)[0] end
local function gc()
  local p = kyubu.symbol("cube::game_controller")
  return p and u32(p) or 0
end

local cur = { frame = 0, dt = 0 }
local prev_dt = 0
local frame_a5, logged, other_diff = nil, 0, 0
local MAX_LINES = 400

kyubu.on("frame.begin", function(ev)
  prev_dt = cur.dt
  cur.frame, cur.dt = ev.frame, ev.dt
  frame_a5 = nil
end)

local hook_id, herr = kyubu.hook_mid(0x4129C0, function(ctx)
  local ok, e = pcall(function()
    local sp = ctx.esp
    local a5 = u32(sp + 20)
    local g = gc()
    if g == 0 then return end
    local rec = ctx.ecx
    local creature = u32(rec)
    local who = (creature == u32(g + GC_PLAYER)) and "player" or "other"
    if who == "player" and frame_a5 == nil then
      frame_a5 = a5
      if logged < MAX_LINES then
        logged = logged + 1
        kyubu.info(("draw_arg5: frame %d dt %.2f prev_dt %.2f a5 %d gc+8006E0 %d gc+2A4 %d caller %08X"):format(
          cur.frame, cur.dt, prev_dt, a5, u32(g + DST), u32(g + SRC), u32(sp)))
      end
    elseif who == "other" and frame_a5 ~= nil and a5 ~= frame_a5 and other_diff < 20 then
      other_diff = other_diff + 1
      kyubu.info(("draw_arg5: frame %d another creature drew with a5 %d, the player with %d, caller %08X"):format(cur.frame, a5, frame_a5, u32(sp)))
    end
  end)
  if not ok and logged < MAX_LINES then logged = logged + 1; kyubu.info("draw_arg5: hook failed: " .. tostring(e)) end
end)
kyubu.info(("draw_arg5: mid hook at 0x4129C0 %s"):format(hook_id and "installed" or ("FAILED: " .. tostring(herr))))
