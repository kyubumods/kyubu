-- Server side: game rules and answers to clients.
kyubu.channel.register("my_lua_mod:ask", function(connection, msg)
  kyubu.channel.send("my_lua_mod:answer", connection, { clicks = msg.clicks + 1 })
end)
