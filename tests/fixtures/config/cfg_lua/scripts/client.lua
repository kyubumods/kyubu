-- Restricted trust: reads and sets its own option; nothing reaches another mod's options.
local r = {}
r[#r + 1] = "get=" .. tostring(kyubu.config.get("volume"))
r[#r + 1] = "set=" .. tostring(kyubu.config.set("volume", 0.25))
r[#r + 1] = "after=" .. tostring(kyubu.config.get("volume"))
local bad, why = kyubu.config.set("volume", 5)
r[#r + 1] = "bad=" .. tostring(bad) .. "/" .. tostring(why)
local n = 0
for _ in pairs(kyubu.config) do n = n + 1 end
r[#r + 1] = "fns=" .. n
local _, other = kyubu.config.get("radius")   -- cfg_demo's option, not this mod's
r[#r + 1] = "other=" .. tostring(other)
kyubu.info("cfg_lua: " .. table.concat(r, " "))
