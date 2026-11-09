-- srefuse, server side: the same ability with a 30 s cooldown. Server.exe uses its own definition, so the
-- client's second start, 4 s after the first, is refused here.
local ID = "srefuse:hit"

kyubu.on("lifecycle.register", function()
  if not kyubu.ability then kyubu.info("srefuse: this loader has no kyubu.ability"); return end
  local ok, err = kyubu.ability.define(ID, {
    duration_ms = 800,
    mp_cost = 0.5,
    max_rank = 3,
    cooldown_ms = function(rank) return 30000 end,
    claim = { slot = 4, class = "warrior", specialization = "any" },
    on_start = function(ctx)
      kyubu.info(("srefuse: on_start number %d rank %d side %s"):format(ctx.number, ctx.rank, ctx.side))
    end,
    on_end = function(ctx)
      kyubu.info(("srefuse: on_end %s side %s, %d ms"):format(ctx.reason, ctx.side, ctx.elapsed_ms))
    end,
  })
  kyubu.info(("srefuse: define: %s"):format(ok and "ok" or tostring(err)))
end)
