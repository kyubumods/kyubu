-- steps_proto, client side (task 4.4 of add-ability-api, probe ability-step-runner). Two abilities whose callbacks only log, so
-- that the replay can count on_start, each on_step and on_end (with the reason), and a mode observer that logs when the player's
-- mode changes (with the mode time and mp). The mage and the rogue have the placeholder (no callbacks: the loader logs its end).
local ffi = require("ffi")

local GC_PLAYER = 0x8006C8
local P_MODE, P_TIME, P_MP = 0x68, 0x6C, 0x170

kyubu.on("lifecycle.register", function()
  if not kyubu.ability then kyubu.info("steps_proto: this loader has no kyubu.ability"); return end
  local ok, err = kyubu.ability.define("steps_proto:burst", {
    duration_ms = 800,
    mp_cost = 0.5,
    max_rank = 1,
    cooldown_ms = function(rank) return 4000 - 500 * rank end,
    claim = { slot = 4, class = "warrior", specialization = "any" },
    on_start = function(ctx)
      ctx.state.n = 0
      kyubu.info(("steps_proto: burst on_start steps %d elapsed %d number %d"):format(ctx.steps, ctx.elapsed_ms, ctx.number))
    end,
    on_step = function(ctx)
      ctx.state.n = ctx.state.n + 1
      kyubu.info(("steps_proto: burst on_step %d elapsed %d dt %d state.n %d"):format(ctx.steps, ctx.elapsed_ms, ctx.dt_ms, ctx.state.n))
    end,
    on_end = function(ctx)
      kyubu.info(("steps_proto: burst on_end %s steps %d elapsed %d state.n %d"):format(ctx.reason, ctx.steps, ctx.elapsed_ms, ctx.state.n))
    end,
  })
  kyubu.info(("steps_proto: define burst: %s"):format(ok and "ok" or tostring(err)))
  ok, err = kyubu.ability.define("steps_proto:fail", {
    duration_ms = 800,
    claim = { slot = 4, class = "ranger", specialization = "any" },
    on_start = function(ctx) kyubu.info("steps_proto: fail on_start") end,
    on_step = function(ctx)
      kyubu.info(("steps_proto: fail on_step %d"):format(ctx.steps))
      if ctx.steps == 2 then error("steps_proto: deliberate error in on_step 2") end
    end,
    on_end = function(ctx) kyubu.info(("steps_proto: fail on_end %s steps %d"):format(ctx.reason, ctx.steps)) end,
  })
  kyubu.info(("steps_proto: define fail: %s"):format(ok and "ok" or tostring(err)))
end)

local function u32(addr) return ffi.cast("uint32_t*", addr)[0] end
local seen = -1
local hook_id, err = kyubu.hook_mid(0x48BFDC, function(ctx)
  local gc = u32(ctx.esp + 0x10)
  local p = gc ~= 0 and u32(gc + GC_PLAYER) or 0
  if p == 0 then return end
  local mode = ffi.cast("uint8_t*", p + P_MODE)[0]
  if mode ~= seen then
    kyubu.info(("steps_proto: mode %d -> %d (mode time %d ms, mp %.3f)"):format(seen, mode, tonumber(u32(p + P_TIME)),
      ffi.cast("float*", p + P_MP)[0]))
    seen = mode
  end
end)
kyubu.info(("steps_proto: observer at 0x48BFDC %s"):format(hook_id and "installed" or ("FAILED: " .. tostring(err))))
