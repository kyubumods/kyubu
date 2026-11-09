-- channel_proto, server side (probe channel-transport-server; add-skills-unleashed-multiplayer 10.4, design D10).
-- On Server.exe: answers each ping on channel_proto:echo with a pong to the connection it came from, echoes the large message, and
-- sends one message to every client after the first ping. A reply the transport cannot send yet (the connection's send thread has
-- not run) is kept and retried at the next tick. Logs "done" when the client acknowledges, so the replay can wait on this log.
-- In single-player (the server side of Cube.exe) nothing reaches it: the client side sends only when joined.
local CH = "channel_proto:echo"

local function log(text) kyubu.info("channel_proto: " .. kyubu.side .. ": " .. text) end

local pending = {}      -- { conn, message }
local pings = {}
local hello_sent = false

local function flush()
  while #pending > 0 do
    local p = pending[1]
    local ok, why = kyubu.channel.send(CH, p[1], p[2])
    if not ok then
      if why ~= "named thing does not exist" then log("send failed: " .. tostring(why)); table.remove(pending, 1) end
      return
    end
    table.remove(pending, 1)
  end
end

local function on_msg(conn, t)
  if type(t) ~= "table" then return log("odd message: " .. tostring(t)) end
  if t.ping then
    pings[#pings + 1] = t.ping
    log(("got ping %d from connection %d"):format(t.ping, conn))
    pending[#pending + 1] = { conn, { pong = t.ping } }
    if not hello_sent then
      hello_sent = true
      pending[#pending + 1] = { kyubu.channel.ALL_CLIENTS, { hello = "server" } }
    end
  elseif t.big then
    log(("got %d bytes from connection %d"):format(#t.big, conn))
    pending[#pending + 1] = { conn, { echo = t.big } }
  elseif t.ack then
    log(("done: pings %s in order, the client saw pongs %s"):format(table.concat(pings, " "), t.ack))
  end
  flush()
end

kyubu.on("lifecycle.register", function()
  local ok, why = kyubu.channel.register(CH, on_msg)
  log("registered: " .. tostring(ok) .. " " .. tostring(why))
  kyubu.on("sim.tick", function()
    local ok2, err = pcall(flush)
    if not ok2 then log("error: " .. tostring(err)) end
  end)
end)
