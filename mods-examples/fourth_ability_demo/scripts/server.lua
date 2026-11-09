-- fourth_ability_demo, server side: the same ability, so that Server.exe numbers it like the client and runs the callbacks for a
-- joined player (add-skills-unleashed-multiplayer 3.2). No icon, clip or slot claim: they are presentation and the client's.
local ID = "fourth_ability_demo:burst"

kyubu.on("lifecycle.register", function()
  if not kyubu.ability then kyubu.info("fourth_ability_demo: this loader has no kyubu.ability"); return end
  local ok, err = kyubu.ability.define(ID, {
    duration_ms = 800,
    mp_cost = 0,
    max_rank = 3,
    cooldown_ms = function(rank) return 6000 - 500 * rank end,
    claim = { slot = 4, class = "warrior", specialization = "any" },

    on_start = function(ctx)
      ctx.state.steps = 0
      kyubu.info(("fourth_ability_demo: on_start %s number %d rank %d side %s"):format(ctx.id, ctx.number, ctx.rank, ctx.side))
    end,
    on_step = function(ctx)
      ctx.state.steps = ctx.state.steps + 1
      kyubu.info(("fourth_ability_demo: on_step %d elapsed %d dt %d"):format(ctx.steps, ctx.elapsed_ms, ctx.dt_ms))
    end,
    on_end = function(ctx)
      kyubu.info(("fourth_ability_demo: on_end %s after %d steps, %d ms"):format(ctx.reason, ctx.state.steps, ctx.elapsed_ms))
    end,
  })
  kyubu.info(("fourth_ability_demo: define: %s"):format(ok and "ok" or tostring(err)))
end)
