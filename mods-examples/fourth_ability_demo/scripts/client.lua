-- fourth_ability_demo: one ability on the warrior's slot 4 (docs/abilities.md). Abilities need full trust: a restricted mod has no
-- kyubu.ability. The callbacks only log; a real ability would use ctx.creature (a game API creature) to act.
local ID = "fourth_ability_demo:burst"

kyubu.on("lifecycle.register", function()
  if not kyubu.ability then kyubu.info("fourth_ability_demo: this loader has no kyubu.ability"); return end
  local ok, err = kyubu.ability.define(ID, {
    icon = "cubeworld:smash",      -- the icon of a vanilla ability: slot 4 draws it
    duration_ms = 800,
    mp_cost = 0,                   -- testing: no mp. The design value is 0.1, a tenth of the mp bar, taken once at the start
    max_rank = 3,
    cooldown_ms = function(rank) return 6000 - 500 * rank end,   -- 6000 ms at rank 0 down to 4500 ms at rank 3
    claim = { slot = 4, class = "warrior", specialization = "any" },
    anim = "fourth_ability_demo:burst",   -- assets/animations/burst.anim.toml: the weapon arm swings over the 800 ms

    on_start = function(ctx)
      ctx.state.steps = 0
      kyubu.info(("fourth_ability_demo: on_start %s number %d rank %d side %s"):format(ctx.id, ctx.number, ctx.rank, ctx.side))
    end,
    on_step = function(ctx)       -- once for each World step, about every 20 ms
      ctx.state.steps = ctx.state.steps + 1
      kyubu.info(("fourth_ability_demo: on_step %d elapsed %d dt %d"):format(ctx.steps, ctx.elapsed_ms, ctx.dt_ms))
    end,
    on_end = function(ctx)        -- reason: done, cancelled, stunned, died, interrupted, gone
      kyubu.info(("fourth_ability_demo: on_end %s after %d steps, %d ms"):format(ctx.reason, ctx.state.steps, ctx.elapsed_ms))
    end,
  })
  kyubu.info(("fourth_ability_demo: define: %s"):format(ok and "ok" or tostring(err)))
end)
