-- charge_proto (task 12.2a of add-skills-unleashed, probe fire-mage-charge). A mod ability's own mode does not grow
-- charged_mp (the game grows it only for hold modes 8, 24, 25, 27, 36, 59, 63, 64). This ability grows it in on_step at the
-- game's rate (0.00075 per ms, at most mp) while key 4 is held (GameController + 4 + bar index 5 = +9, run 2 of the probe), and spends it on release.
-- It logs, every 250 ms of the ability: the mode, the key byte, charged_mp, mp, so the log shows whether the game keeps
-- the mode and the callbacks running while the key is held, and whether it touches the value we wrote.
local ffi = require("ffi")
local ID = "charge_proto:circle"
local GC_PLAYER, GC_KEY4 = 0x8006C8, 9
local P_MODE, P_CHARGED, P_MP = 0x68, 0x134, 0x170
local RATE = 0.00075

local function u32(a) return ffi.cast("uint32_t*", a)[0] end
local function u8(a) return ffi.cast("uint8_t*", a)[0] end
local function f32(a) return ffi.cast("float*", a)[0] end
local function put(a, v) ffi.cast("float*", a)[0] = v end

local function addresses()
  local g = kyubu.symbol("cube::game_controller")
  local gc = g and u32(g) or 0
  local p = gc ~= 0 and u32(gc + GC_PLAYER) or 0
  return gc, p
end

local last_ms, last_log, held_before = 0, -1000, true

kyubu.on("lifecycle.register", function()
  local ok, err = kyubu.ability.define(ID, {
    icon = "cubeworld:smash",
    duration_ms = 20000,
    mp_cost = 0,
    max_rank = 1,
    cooldown_ms = function(rank) return 60000 end,
    claim = { slot = 4, class = "warrior", specialization = "any" },
    on_start = function(ctx)
      local gc, p = addresses()
      if p ~= 0 then put(p + P_MP, 1); put(p + P_CHARGED, 0) end
      last_ms, last_log, held_before = 0, -1000, true
      kyubu.info("charge_proto: on_start, mp set to 1, charged_mp 0")
    end,
    on_step = function(ctx)
      local gc, p = addresses()
      if p == 0 then return end
      local held = u8(gc + GC_KEY4) ~= 0
      local dt = ctx.elapsed_ms - last_ms
      last_ms = ctx.elapsed_ms
      if held then
        put(p + P_CHARGED, math.min(f32(p + P_CHARGED) + RATE * dt, f32(p + P_MP)))
      end
      if ctx.elapsed_ms - last_log >= 250 then
        last_log = ctx.elapsed_ms
        kyubu.info(("charge_proto: step %d t=%d ms mode=%d key=%d charged=%.4f mp=%.4f"):format(
          ctx.steps, ctx.elapsed_ms, u8(p + P_MODE), u8(gc + GC_KEY4), f32(p + P_CHARGED), f32(p + P_MP)))
        kyubu.info(("charge_proto: key bytes +4..+10: %d %d %d %d %d %d %d"):format(u8(gc+4),u8(gc+5),u8(gc+6),u8(gc+7),u8(gc+8),u8(gc+9),u8(gc+10)))
      end
      if ctx.elapsed_ms > 2700 then
        kyubu.info(("charge_proto: late step %d t=%d mode=%d mode_time=%d stun=%d hp=%.2f"):format(
          ctx.steps, ctx.elapsed_ms, u8(p + P_MODE), u32(p + 0x6C), u32(p + 0x12C), f32(p + 0x10C)))
      end
      if held_before and not held then
        local charged = f32(p + P_CHARGED)
        put(p + P_MP, f32(p + P_MP) - charged); put(p + P_CHARGED, 0)
        kyubu.info(("charge_proto: released at %d ms: spent %.4f, mp now %.4f"):format(ctx.elapsed_ms, charged, f32(p + P_MP)))
        kyubu.game.ability_end(ctx.creature)
      end
      held_before = held
    end,
    on_end = function(ctx)
      kyubu.info(("charge_proto: on_end %s after %d steps, %d ms"):format(ctx.reason, ctx.steps, ctx.elapsed_ms))
    end,
  })
  kyubu.info(("charge_proto: define %s: %s"):format(ID, ok and "ok" or tostring(err)))
end)
