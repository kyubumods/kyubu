-- entity_spawner, server side: spawns what the client side asks for with kyubu.game.creature_spawn (the loader runs the
-- game's own recipe) and replies with the ids. It keeps the creatures it spawned and logs when the game removes one
-- (killed, or left behind), as their objects report it.

-- A request: { what = "3 x Green slime", hostile =, level =, behavior =, owner =, list = { {type =, x =, y =, z =, yaw =},
-- ... } }, positions in world units, yaw in degrees. `behavior` is one of the kinds below (the game's "default" when
-- it is absent); a companion follows the creature whose game id is `owner` (the client side sends the player's, which
-- only it has a handle of). The layout commands of the mapping probes ask for "still".
local BEHAVIORS = { default = true, still = true, monster = true, friendly = true, companion = true, guard = true }
local PATROL = 5   -- blocks: half the side of the square a guard walks around its spawn point
local player_id            -- the game id of the player, from the last request that carried it
local spawned = {}   -- creature object -> { label =, id =, hp = } of each creature this side spawned and still watches

-- One creature; the creature and its state, or nil and the reason. A field of the wrong type raises in creature_spawn,
-- and a message from a client is not to be trusted, so it runs under pcall.
local function spawn_one(s, hostile, level, behavior, owner)
  local path
  if behavior == "guard" then
    local r = PATROL * UNITS_PER_BLOCK
    path = { { s.x - r, s.y - r, s.z }, { s.x + r, s.y - r, s.z }, { s.x + r, s.y + r, s.z }, { s.x - r, s.y + r, s.z } }
  end
  local ok, creature, why = pcall(kyubu.game.creature_spawn, {
    type = s.type, hostile = hostile, level = level, pos = { x = s.x, y = s.y, z = s.z }, yaw = s.yaw,
    behavior = behavior, owner = owner, path = path })
  if not ok then return nil, tostring(creature) end
  if not creature then return nil, why end
  local info, gone = creature:get()
  if not info then return nil, gone end
  return creature, info
end

local function label_of(type_value)
  for _, t in ipairs(entity_types() or {}) do
    if t.value == type_value then return t.readable end
  end
  return "type " .. tostring(type_value)
end

kyubu.channel.register("entity_spawner:spawn", function(conn, m)
  if type(m) ~= "table" or type(m.list) ~= "table" or #m.list == 0 then
    kyubu.channel.send("entity_spawner:result", conn, { ok = false, text = "not a spawn request" })
    return
  end
  player_id = tonumber(m.owner) or player_id
  local behavior = m.behavior == nil and "default" or m.behavior
  if type(behavior) ~= "string" or not BEHAVIORS[behavior] then
    kyubu.channel.send("entity_spawner:result", conn, { ok = false, text = "unknown behavior: " .. tostring(behavior) })
    return
  end
  local owner
  if behavior == "companion" then
    local why
    owner, why = kyubu.game.creature_find(tonumber(m.owner) or 0)
    if not owner then
      local text = "a companion needs an owner: " .. tostring(why)
      kyubu.warn(text)
      kyubu.channel.send("entity_spawner:result", conn, { ok = false, text = text })
      return
    end
  end
  local ids, labels, failed = {}, {}, nil
  for _, s in ipairs(m.list) do
    local creature, info = spawn_one(s, m.hostile, m.level, behavior, owner)
    if creature then
      local label = label_of(info.type)
      spawned[creature] = { label = label, id = info.id, hp = info.hp, behavior = behavior }
      ids[#ids + 1], labels[#labels + 1] = info.id, label
      kyubu.info(("spawned %s (type %d, hostile %d, level %d, behavior %s), id %d, hp %.1f at %.0f %.0f %.0f, yaw %.1f: %s")
        :format(label, info.type, info.hostile, info.level, behavior, info.id, info.hp, info.pos.x, info.pos.y,
          info.pos.z, info.yaw, tostring(creature)))
    else
      failed = failed or info
      kyubu.warn(("not spawned (type %s): %s"):format(tostring(s.type), tostring(info)))
    end
  end
  local text
  if #ids == 0 then
    text = "not spawned: " .. tostring(failed)
  elseif #ids <= 3 then
    local each = {}
    for i, id in ipairs(ids) do each[i] = ("%s, id %d"):format(labels[i], id) end
    text = "spawned " .. table.concat(each, "; ")
  else
    text = ("spawned %d creatures, ids %d to %d"):format(#ids, ids[1], ids[#ids])
  end
  if failed and #ids > 0 then text = ("%s (%d not spawned: %s)"):format(text, #m.list - #ids, failed) end
  kyubu.info(("%s: %s"):format(tostring(m.what), text))
  kyubu.channel.send("entity_spawner:result", conn, { ok = #ids > 0, text = text })
end)

-- How far a creature is from the player, in blocks, and the player's position; nil when the player is not known (or
-- gone).
local function blocks_from_player(info)
  local player = player_id and kyubu.game.creature_find(player_id)
  local p = player and player:get()
  if not p then return nil end
  return math.sqrt((info.pos.x - p.pos.x) ^ 2 + (info.pos.y - p.pos.y) ^ 2) / UNITS_PER_BLOCK, p.pos
end

-- Every second (ten ticks), each spawned creature: a change of hp is logged, and once the game has removed the creature
-- (at hp 0 it dies; left far behind it despawns) its object says it is gone; the distance to the player seen last says
-- which. valid() asks without a refusal in the log. With the option `trace`, every fifth second also logs where each
-- one is (how the behaviours are told apart in a replay).
local ticks = 0

kyubu.on("sim.tick", function()
  ticks = ticks + 1
  if ticks % 10 ~= 0 then return end
  local trace = ticks % 50 == 0 and kyubu.config.get("trace") == true
  for creature, last in pairs(spawned) do
    if not creature:valid() then
      kyubu.info(("%s (id %d) is gone: %s; last hp %.1f, last seen %s blocks from the player"):format(last.label, last.id,
        tostring(creature), last.hp, last.dist and ("%.1f"):format(last.dist) or "?"))
      spawned[creature] = nil
    else
      local info = creature:get()
      if info then
        local player_pos
        last.dist, player_pos = blocks_from_player(info)
        if trace then
          kyubu.info(("trace %s (id %d, %s): %.1f blocks from the player, at %.0f %.0f %.0f, hp %.1f; player at %s"):format(
            last.label, last.id, last.behavior, last.dist or -1, info.pos.x, info.pos.y, info.pos.z, info.hp,
            player_pos and ("%.0f %.0f %.0f"):format(player_pos.x, player_pos.y, player_pos.z) or "?"))
        end
      end
      if info and info.hp ~= last.hp then
        kyubu.info(("%s (id %d): hp %.1f -> %.1f"):format(last.label, last.id, last.hp, info.hp))
        last.hp = info.hp
      end
    end
  end
end)
