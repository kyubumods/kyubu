-- anim_signals_proto, client side (task 8.3 of add-ability-animation, probe anim-signals). The clip `charge` has `expr` tracks that read the
-- draw's signals. The mod puts it on the local player with the preview clock, so the clip is worn whatever the player does, and the player's own
-- vanilla mode (a held right button is a hold mode, which grows charged_mp) drives the signal. mp is kept at 1 so that the charge has room to grow.
-- A mid hook at the join site of draw_creature (0x41F135) logs, for the local player, the creature's charged_mp, its velocity and the vectors the draw
-- reads there, when the charge moved by 0.05 or every two seconds.
local ffi = require("ffi")

local JOIN = 0x41F135
local GC_PLAYER, P_MP, P_CHARGED, P_VEL, P_MODE = 0x8006C8, 0x170, 0x144, 0x34, 0x68   -- creature offsets (cuwo + 0x10)
local HAND_R_ROT, HEAD_ROT = 0x30, 0x84                                              -- the draw record's vectors (three floats each)
local WARMUP = 60
local CLIP = "anim_signals_proto:charge"

local function rd32(addr)
  local bytes = kyubu.read(addr, 4)
  return bytes and ffi.cast("uint32_t*", bytes)[0] or nil
end
local function player()
  local p = kyubu.symbol("cube::game_controller")
  local gc = p and rd32(p) or 0
  return (gc and gc ~= 0) and (rd32(gc + GC_PLAYER) or 0) or 0
end

local t0, on, last_charge, last_log = nil, false, -1, 0
local id_j, err_j = kyubu.hook_mid(JOIN, function(ctx)
  pcall(function()
    local rec = ctx.edi
    local creature = ffi.cast("uint32_t*", rec)[0]
    if creature == 0 or creature ~= player() then return end
    local now = os.time()
    if not t0 then t0 = now; kyubu.info("animsig: first draw of the player") end
    if now - t0 < WARMUP then return end
    if not on then
      on = true
      local ok, why = kyubu.game.animation_preview(CLIP, 300)
      kyubu.info(("animsig: preview on %s %s"):format(tostring(ok), tostring(why or "")))
      return
    end
    ffi.cast("float*", creature + P_MP)[0] = 1.0
    local charged = ffi.cast("float*", creature + P_CHARGED)[0]
    if math.abs(charged - last_charge) >= 0.05 or now - last_log >= 2 then
      last_charge, last_log = charged, now
      local h, hd = ffi.cast("float*", rec + HAND_R_ROT), ffi.cast("float*", rec + HEAD_ROT)
      local v = ffi.cast("float*", creature + P_VEL)
      kyubu.info(("animsig: draw mode %d charged %.4f velocity %.2f %.2f %.2f hand_r rot %.3f %.3f %.3f head rot %.3f %.3f %.3f"):format(
        ffi.cast("uint8_t*", creature + P_MODE)[0], charged, v[0], v[1], v[2], h[0], h[1], h[2], hd[0], hd[1], hd[2]))
    end
  end)
end)
kyubu.info(("animsig: mid hook at the join site 0x%X %s"):format(JOIN, id_j and "installed" or ("FAILED: " .. tostring(err_j))))
