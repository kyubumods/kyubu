local n = 0
kyubu.on("frame.begin", function() n = n + 1; if n == 30 then kyubu.info("hot version 1") end end)
kyubu.on_unload(function() kyubu.info("unloading version 1") end)
