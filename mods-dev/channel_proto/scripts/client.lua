-- channel_proto, client side (probe channel-transport-server; add-skills-unleashed-multiplayer 10.4, design D10).
-- On a Cube.exe joined to a Kyubu Server.exe: sends pings 1, 2 and 3 and one large message (60,000 bytes of text) on
-- channel_proto:echo, then checks what comes back: the pongs in order, the large message echoed byte for byte, and the server's one
-- message to every client. When all of it is in, it logs "done" and tells the server (the replay waits on the server's log).
-- In single-player it sends nothing (the channel is loopback there and has its own tests).
local CH = "channel_proto:echo"
local BIG = 60000

local function log(text) kyubu.info("channel_proto: " .. kyubu.side .. ": " .. text) end

local big = string.rep("0123456789", BIG / 10)
local sent, tries, frames = false, 0, 0
local pongs, big_ok, hello, finished = {}, false, false, false

local function check_done()
  if finished or #pongs < 3 or not big_ok or not hello then return end
  finished = true
  local order = table.concat(pongs, " ")
  log("done: pongs " .. order .. ", large echo ok, hello ok")
  kyubu.channel.send(CH, kyubu.channel.SERVER, { ack = order })
end

local function on_msg(conn, t)
  if type(t) ~= "table" then return log("odd message: " .. tostring(t)) end
  if t.pong then
    pongs[#pongs + 1] = t.pong
    log(("got pong %d from connection %d"):format(t.pong, conn))
  elseif t.echo then
    big_ok = t.echo == big
    log(("got the large echo: %d bytes, %s"):format(#t.echo, big_ok and "equal" or "DIFFERENT"))
  elseif t.hello then
    hello = true
    log("got hello to all clients: " .. tostring(t.hello))
  end
  check_done()
end

local function try_send()
  if sent or kyubu.host().mode ~= "remote_client" then return end
  frames = frames + 1
  if frames % 30 ~= 1 then return end
  local ok, why = kyubu.channel.send(CH, kyubu.channel.SERVER, { ping = 1 })
  if not ok then
    tries = tries + 1
    if tries == 1 then log("cannot send yet: " .. tostring(why)) end
    return
  end
  kyubu.channel.send(CH, kyubu.channel.SERVER, { ping = 2 })
  kyubu.channel.send(CH, kyubu.channel.SERVER, { ping = 3 })
  local ok2, why2 = kyubu.channel.send(CH, kyubu.channel.SERVER, { big = big })
  sent = true
  log("sent pings 1 2 3 and " .. BIG .. " bytes: " .. tostring(ok2) .. " " .. tostring(why2))
end

kyubu.on("lifecycle.register", function()
  local ok, why = kyubu.channel.register(CH, on_msg)
  log("registered: " .. tostring(ok) .. " " .. tostring(why))
  kyubu.on("frame.begin", function()
    local ok3, err = pcall(try_send)
    if not ok3 then log("error: " .. tostring(err)) end
  end)
end)
