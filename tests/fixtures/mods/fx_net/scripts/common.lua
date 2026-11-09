local recv, recv_bytes, sent, sent_bytes, threads = 0, 0, 0, 0, {}
kyubu.on("net.connect", function(ev)
  kyubu.info(("net.connect %s:%s (network thread: %s)"):format(tostring(ev.address), tostring(ev.port), tostring(ev.network_thread)))
end)
kyubu.on("net.recv_raw", function(ev)
  recv, recv_bytes = recv + 1, recv_bytes + #ev.data
  threads[ev.network_thread and "network" or "main"] = true
  if recv == 1 then kyubu.info(("first net.recv_raw: %d bytes, starts %02X %02X %02X %02X"):format(#ev.data,
    ev.data:byte(1) or 0, ev.data:byte(2) or 0, ev.data:byte(3) or 0, ev.data:byte(4) or 0)) end
end)
kyubu.on("net.send_raw", function(ev) sent, sent_bytes = sent + 1, sent_bytes + #ev.data end)
local last = 0
local function report(now)
  if now - last < 10000 then return end
  last = now
  if recv + sent > 0 then
    local t = {}
    for k in pairs(threads) do t[#t + 1] = k end
    kyubu.info(("net: %d recv (%d bytes), %d send (%d bytes), delivered on: %s"):format(recv, recv_bytes, sent, sent_bytes, table.concat(t, ",")))
  end
end
local clock = 0
kyubu.on("frame.begin", function(ev) clock = clock + (ev.dt or 0); report(clock) end, 0, "client")
