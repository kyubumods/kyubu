-- markers_proto, client side (task 7.8 of add-ability-animation, probe anim-markers). One ability on the warrior's slot 4 whose clip
-- has four markers. Every callback that is told a marker logs the names, in the order ctx:marker() gave them, with the step's mode time.
local function names(ctx)
  return table.concat(ctx:marker(), ",")
end

kyubu.on("lifecycle.register", function()
  if not kyubu.ability then kyubu.info("markers_proto: this loader has no kyubu.ability"); return end
  local ok, err = kyubu.ability.define("markers_proto:swing", {
    duration_ms = 800,
    mp_cost = 0,
    max_rank = 1,
    cooldown_ms = function(rank) return 3000 end,
    claim = { slot = 4, class = "warrior", specialization = "any" },
    anim = "markers_proto:swing",
    on_start = function(ctx)
      kyubu.info(("markers_proto: on_start elapsed %d markers %d"):format(ctx.elapsed_ms, #ctx:marker()))
    end,
    on_step = function(ctx)
      local n = names(ctx)
      if n ~= "" then
        kyubu.info(("markers_proto: on_step %d elapsed %d dt %d markers [%s]"):format(ctx.steps, ctx.elapsed_ms, ctx.dt_ms, n))
      end
    end,
    on_end = function(ctx)
      kyubu.info(("markers_proto: on_end %s steps %d elapsed %d markers [%s]"):format(ctx.reason, ctx.steps, ctx.elapsed_ms, names(ctx)))
    end,
  })
  kyubu.info(("markers_proto: define: %s"):format(ok and "ok" or tostring(err)))
end)
