-- procedural models: any key proc-*.cub is generated on demand
kyubu.assets.provider("data1.db", "proc-*.cub", function(db, key)
  local sx, sy, sz = 4, 4, 12
  local parts = { string.char(sx, 0, 0, 0, sy, 0, 0, 0, sz, 0, 0, 0) }
  for z = 0, sz - 1 do for y = 0, sy - 1 do for x = 0, sx - 1 do
    parts[#parts + 1] = (z < 8) and string.char(90, 60, 30) or string.char(40, 160, 40)
  end end end
  kyubu.info("provider generated " .. key)
  return table.concat(parts)
end)

-- a Lua function hook through the FFI (Database::get is a thiscall)
local reads = 0
local id, err = kyubu.hook("Database::get", "bool(__thiscall*)(void*, void*, void**, uint32_t*)",
  function(next, self, key, out, len)
    reads = reads + 1
    return next(self, key, out, len)
  end)
kyubu.info("lua hook installed: " .. tostring(id) .. " " .. tostring(err))

-- a mid-function hook with register access (runs when the model table is built)
local mid, merr = kyubu.hook_mid("ModelLoader::resizeSite", function(ctx)
  kyubu.info(("mid hook at 0x%X, esp 0x%X"):format(ctx.eip, ctx.esp))
end)
kyubu.info("mid hook installed: " .. tostring(mid) .. " " .. tostring(merr))

kyubu.on("lifecycle.game_ready", function()
  kyubu.info("Database::get calls seen by the Lua hook: " .. reads)
  local iface, ver = kyubu.interface("hello_native:counter", 1)
  kyubu.info("hello_native:counter interface: " .. tostring(iface) .. " v" .. tostring(ver))
  local data, layer = kyubu.assets.read("data1.db", "proc-tree7.cub")
  kyubu.info("proc-tree7.cub: " .. tostring(data and #data) .. " bytes from " .. tostring(layer))
  kyubu.info("iron-sword1.cub served by " .. kyubu.assets.served_by("data1.db", "iron-sword1.cub"))
end)

kyubu.command("fx_features:reads", "Lua hook call count", function(args, print) print("reads=" .. reads) end)
