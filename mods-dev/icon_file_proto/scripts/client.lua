-- icon_file_proto (task 8.3 of add-ability-api): one ability on the warrior's slot 4 with its own icon. icon_file names a PNG of the
-- mod's assets/icons/ folder (data3.db); icon is the vanilla fallback when the file cannot be loaded.
kyubu.on("lifecycle.register", function()
  local ok, err = kyubu.ability.define("icon_file_proto:burst", {
    icon = "cubeworld:smash",
    icon_file = "proto-burst.png",
    duration_ms = 800,
    mp_cost = 0.1,
    max_rank = 3,
    cooldown_ms = function(rank) return 6000 - 500 * rank end,
    claim = { slot = 4, class = "warrior", specialization = "any" },
  })
  kyubu.info("icon_file_proto: define: " .. (ok and "ok" or tostring(err)))
end)

-- Task 8.3 investigation (probe ability-icon-compare): why is the icon loaded by Kyubu softer than the game's own? Once, 25 s after the
-- bar has number 112, dump the icon object of ability 54 (smash, loaded by the game's constructor) and of 112 (loaded by Kyubu), then
-- every pointer in each object (one level) as hex, to compare the two in the log.
local ffi = require("ffi")
local function rd32(addr)
  local b = kyubu.read(addr, 4)
  return b and ffi.cast("uint32_t*", b)[0] or nil
end
local function hex(addr, n)
  local b = kyubu.read(addr, n)
  if not b then return "unreadable" end
  local t = {}
  for i = 1, #b, 4 do
    local w = {b:byte(i, i + 3)}
    t[#t + 1] = ("%02x%02x%02x%02x"):format(w[4] or 0, w[3] or 0, w[2] or 0, w[1] or 0)
  end
  return table.concat(t, " ")
end
local function node_value(map, key)
  local head = rd32(map)
  local node = head and rd32(head + 4)
  for _ = 1, 256 do
    if not node then return nil end
    local flag = kyubu.read(node + 0xD, 1)
    if not flag or flag:byte(1) ~= 0 then return nil end
    local k = rd32(node + 0x10)
    if k == key then return rd32(node + 0x14) end
    node = (k and key < k) and rd32(node) or rd32(node + 8)
  end
end
local function dump(label, obj)
  kyubu.info(("icondump %s object 0x%X: %s"):format(label, obj, hex(obj, 0x60)))
  for off = 0, 0x5C, 4 do
    local p = rd32(obj + off)
    if p and p > 0x10000 and p < 0x7FFF0000 and p ~= obj then
      kyubu.info(("icondump %s +0x%02X -> 0x%X: %s"):format(label, off, p, hex(p, 0x80)))
    end
  end
end
local done, first = false, nil
kyubu.on("frame.begin", function()
  if done then return end
  first = first or os.time()
  if os.time() - first < 25 then return end   -- 25 s after the first frame
  local g = kyubu.symbol("cube::game_controller")
  local gc = g and rd32(g)
  if not gc or gc == 0 then return end
  local vanilla, mine = node_value(gc + 0x800818, 54), node_value(gc + 0x800818, 112)
  if not vanilla or not mine or vanilla == 0 or mine == 0 then return end
  done = true
  dump("vanilla54", vanilla)
  dump("kyubu112", mine)
end)
