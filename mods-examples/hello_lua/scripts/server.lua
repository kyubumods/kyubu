local ticks = 0
kyubu.on("sim.tick", function() ticks = ticks + 1 end)

kyubu.channel.register("hello_lua:ping", function(conn, msg)
  kyubu.channel.send("hello_lua:pong", conn, { n = msg.n, ticks = ticks })
end)
