-- srefuse, client side: a short cooldown, so the client starts the ability again where the server will refuse it.
local ID = "srefuse:hit"

kyubu.on("lifecycle.register", function()
  if not kyubu.ability then kyubu.info("srefuse: this loader has no kyubu.ability"); return end
  local ok, err = kyubu.ability.define(ID, {
    icon = "cubeworld:smash",
    duration_ms = 800,
    mp_cost = 0.5,
    max_rank = 3,
    cooldown_ms = function(rank) return 1000 end,
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

-- The replay cannot type a console command reliably under software rendering: the mod sets its own rank (3) once the played
-- character exists, and logs it.
local ranked = false
kyubu.on("frame.begin", function()
  if ranked or not kyubu.game or not kyubu.game.ability_rank_set then return end
  local r = kyubu.game.ability_rank_set(ID, 3)
  if r then
    ranked = true
    kyubu.info(("srefuse: rank %d set"):format(r))
  end
end)
