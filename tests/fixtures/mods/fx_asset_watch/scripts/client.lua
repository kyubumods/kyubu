local n, first, per_db = 0, nil, {}
kyubu.on("asset.request", function(ev)
  n = n + 1
  per_db[ev.db] = (per_db[ev.db] or 0) + 1
  if not first then first = ("%s/%s in phase %s"):format(ev.db, ev.key, kyubu.phase()) end
end)
kyubu.on("lifecycle.game_ready", function()
  local parts = {}
  for db, c in pairs(per_db) do parts[#parts + 1] = db .. "=" .. c end
  table.sort(parts)
  kyubu.info(("asset.request: %d events (%s), first: %s"):format(n, table.concat(parts, " "), tostring(first)))
end)
