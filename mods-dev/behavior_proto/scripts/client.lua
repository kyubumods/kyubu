-- behavior_proto, client side: the console command `behavior_proto:slimes [blocks]` asks the server side for three
-- green slimes north of the player: a hostile one without behaviour (the control), a friendly one with the zone's
-- default behaviour, and a hostile one with it (spawned last, so it has the lowest id). `behavior_proto:monster
-- [blocks]` spawns only the last one. Positions are world units, 65,536 to the block; y is north.

local UNITS_PER_BLOCK = 65536

local function request(kind, blocks)
  local p, why = kyubu.game.creature_player()
  local info
  if p then info, why = p:get() end
  if not info then return nil, "no player: " .. tostring(why) end
  local ok, sent = kyubu.channel.send("behavior_proto:spawn", kyubu.channel.SERVER, {
    kind = kind, x = info.pos.x, y = info.pos.y, z = info.pos.z + UNITS_PER_BLOCK, blocks = blocks, level = info.level })
  if not ok then return nil, tostring(sent) end
  return true
end

kyubu.command("behavior_proto:slimes", "behavior_proto:slimes [blocks]: control, friendly and hostile slime with behaviour",
  function(args, print)
    local ok, why = request("all", tonumber(args) or 10)
    print(ok and "asked for three slimes" or ("not sent: " .. why))
  end)

kyubu.command("behavior_proto:monster", "behavior_proto:monster [blocks]: one hostile slime with the default behaviour",
  function(args, print)
    local ok, why = request("monster", tonumber(args) or 10)
    print(ok and "asked for a monster" or ("not sent: " .. why))
  end)

-- Task 8.2: `behavior_proto:cases` (every behaviour class on a friendly slime, one each), `behavior_proto:combat` (a
-- hostile slime with a lone CombatBehavior, killed by the mod after 35 s) and `behavior_proto:kill` (hp 0 for all).
local function request_cases(kind)
  local p, why = kyubu.game.creature_player()
  local info
  if p then info, why = p:get() end
  if not info then return nil, "no player: " .. tostring(why) end
  local ok, sent = kyubu.channel.send("behavior_proto:cases", kyubu.channel.SERVER, {
    kind = kind, x = info.pos.x, y = info.pos.y, z = info.pos.z + UNITS_PER_BLOCK, level = info.level, player_id = info.id })
  if not ok then return nil, tostring(sent) end
  return true
end

for name, kind in pairs({ cases = "all", combat = "combat", kill = "kill" }) do
  kyubu.command("behavior_proto:" .. name, "behavior_proto:" .. name .. ": task 8.2, see the script", function(_, print)
    local ok, why = request_cases(kind)
    print(ok and ("asked: " .. kind) or ("not sent: " .. why))
  end)
end
