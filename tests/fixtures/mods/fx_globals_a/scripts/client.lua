shared = "a"
kyubu.on("lifecycle.game_ready", function() kyubu.info("my global 'shared' is " .. tostring(shared)) end)
