-- entity_spawner, client side: the window, its key, and the console command `entity_spawner:spawner`. Only the server
-- side can spawn (kyubu.game.creature_spawn belongs to it), so the client reads the player through the game API, works
-- out where each creature goes, and sends that to the server side over a channel.

-- body_yaw is in degrees counter-clockwise from north (+y): a creature faces (-sin yaw, cos yaw) (probe creature-units).
-- cube::HostileType: hostile, friendly (nameless, on the player's side), named friendly (the game's summon for a player
-- and its villagers: a name and a talk prompt)
local HOSTILE, FRIENDLY, NAMED = 1, 2, 3
local MAX_COUNT = 20             -- the most `spawner <type> [count]` asks for
local COLUMNS = 3                -- buttons per row in the window
local BEHAVIORS = { "default", "still", "monster", "friendly", "companion", "guard" }   -- the option's choices
local USAGE = "usage: spawner [<type|name> [count] [behavior] [named|unnamed] | all [first [count]] | list [filter] | where [seconds] | " ..
              "ring [type] | hostile <hostile_type> [type] | facing [type] | ruler [type] | behaviors [type]]"

local open = false
local filter = ""
local level = tonumber(kyubu.config.get("level")) or 0
local hostile = kyubu.config.get("hostile") ~= false
local named = kyubu.config.get("named") == true
local behavior  -- the window's choice: the option, with the old `fights = false` still meaning "still"
local status = "Click a type to spawn it in front of you."
local note_listed = true   -- log how many types the window lists at its next frame (after opening or a new filter)

-- ---------------------------------------------------------------- the player and the server side

-- The player's state through the game API: { id, type, hostile, level, hp, pos = {x, y, z}, yaw }, or nil and the reason.
local function player()
  local p, why = kyubu.game.creature_player()
  if not p then return nil, why end
  return p:get()
end

local function is_behavior(word)
  for _, b in ipairs(BEHAVIORS) do
    if b == word then return true end
  end
  return false
end

-- The behaviour the window and `spawner` use when none is named: the option (`default`, unless a config from before the
-- option says `fights = false`).
local function configured_behavior()
  local b = kyubu.config.get("behavior")
  if not is_behavior(b) then b = "default" end
  if b == "default" and kyubu.config.get("fights") == false then b = "still" end
  return b
end
behavior = configured_behavior()

local function say(text)
  status = text
  kyubu.info(text)
end

-- The hostile type a spawn of this behaviour gets. The player cannot hurt a creature of the player's side, and the
-- hostile creatures fight it. Whether it has a name and can be talked to is the type's doing (the game shows the talk
-- prompt for type 3 only), so `is_named` picks between the two: 3 (named, as the game's villagers and summons) or 2
-- (nameless). A friendly and a companion creature are always on the player's side. (The game's own pet is type 5, but
-- the game kills a type-5 creature that is not the one pet its owner has registered, so a spawn cannot be one.) The
-- hostile checkbox decides for the other kinds (default, still, monster, guard): on, hostile (1); off, the same two.
local function hostility_for(kind, is_named)
  if is_named == nil then is_named = named end
  if kind == "companion" or kind == "friendly" or not hostile then return is_named and NAMED or FRIENDLY end
  return HOSTILE
end

-- Whether the hostile checkbox means anything for this behaviour.
local function chooses_hostility(kind)
  return kind ~= "companion" and kind ~= "friendly"
end

-- Whether the named setting means anything: not for a creature that is hostile.
local function chooses_name(kind)
  return not (chooses_hostility(kind) and hostile)
end

-- Sends one request to the server side: `list` holds { type =, x =, y =, z =, yaw = } in world units and degrees;
-- `behavior_kind` is one of BEHAVIORS, and a companion follows the creature with the game id `owner_id` (the player: only
-- this side has a handle of it, so the server side finds it by id).
-- On a joined client the send fails with "remote does not support channels" (Kyubu has no channel transport over the
-- network yet), so there the server side never gets the request.
local function request(what, list, hostile_type, lvl, behavior_kind, owner_id)
  local ok, why = kyubu.channel.send("entity_spawner:spawn", kyubu.channel.SERVER,
    { what = what, hostile = hostile_type, level = lvl, list = list, behavior = behavior_kind, owner = owner_id })
  if ok then
    say(("asked for %s"):format(what))
  elseif why == "remote does not support channels" then
    say("The server does not run the entity spawner: nothing was spawned.")
  else
    say(("not sent: %s"):format(tostring(why)))
  end
end

kyubu.channel.register("entity_spawner:result", function(_, msg)
  say(("server side: %s"):format(tostring(msg.text)))
end)

-- The level to spawn at: the chosen one, or the player's when that is 0.
local function spawn_level(info)
  return level >= 1 and level or math.max(1, info.level)
end

-- `count` creatures of type t in a line in front of the player, the first `distance` blocks away and then every two
-- blocks, one block up (they fall to the ground), facing the player. `kind` is the behaviour and `is_named` the
-- setting of a friendly or companion creature (the window's when nil).
local function spawn(t, count, kind, is_named)
  local info, why = player()
  if not info then return say("no player: " .. tostring(why)) end
  local distance = tonumber(kyubu.config.get("distance")) or 4
  local rad = math.rad(info.yaw)
  local list = {}
  for i = 1, count do
    local ahead = (distance + (i - 1) * 2) * UNITS_PER_BLOCK
    list[i] = { type = t.value, x = info.pos.x - math.sin(rad) * ahead, y = info.pos.y + math.cos(rad) * ahead,
                z = info.pos.z + UNITS_PER_BLOCK, yaw = info.yaw + (info.yaw > 0 and -180 or 180) }
  end
  kind = kind or behavior
  request(("%d x %s (%s)"):format(count, t.label, kind), list, hostility_for(kind, is_named), spawn_level(info), kind,
          info.id)
end

-- Creatures on the world axes around the player: `places` holds { east, north, yaw } in blocks and degrees (yaw nil:
-- the player's), `types` one entity-type value for all or a list with one per place. The layout does not depend on the
-- player's facing, which the mapping probes' screenshots need. Their creatures stand still.
local function place(what, types, hostile_type, places, kind)
  local info, why = player()
  if not info then return say("no player: " .. tostring(why)) end
  local list = {}
  for i, p in ipairs(places) do
    list[i] = { type = type(types) == "table" and types[i] or types, x = info.pos.x + p[1] * UNITS_PER_BLOCK,
                y = info.pos.y + p[2] * UNITS_PER_BLOCK, z = info.pos.z + UNITS_PER_BLOCK, yaw = p[3] or info.yaw }
  end
  request(what, list, hostile_type, spawn_level(info), kind or "still", info.id)
end

-- `spawner all`: one creature of every type on a square grid four blocks apart, centred on the player (its own place
-- left free). `spawner all <first> [count]`: `count` types (20) from the first-th on, in rows of five north of the player,
-- where the camera of a new world looks (probe entity-types).
local function all(rest)
  local types, why = entity_types()
  if not types then return say("no entity types: " .. tostring(why)) end
  local first, count = rest:match("^(%d*)%s*(%d*)$")
  if not first then return say(USAGE) end
  local values, places = {}, {}
  if first == "" then
    local side = math.ceil(math.sqrt(#types + 1))
    local half = math.floor(side / 2)
    for row = 0, side - 1 do
      for col = 0, side - 1 do
        local t = types[#values + 1]
        if t and not (row == half and col == half) then
          values[#values + 1] = t.value
          places[#places + 1] = { (col - half) * 4, (row - half) * 4 }
        end
      end
    end
    return place(("all %d entity types"):format(#values), values, hostile and HOSTILE or FRIENDLY, places)
  end
  first, count = tonumber(first), tonumber(count) or 20
  for i = 0, count - 1 do
    local t = types[first + i + 1]
    if not t then break end
    local ahead = 6 + math.floor(i / 5) * 5
    values[#values + 1] = t.value
    places[#places + 1] = { (i % 5 - 2) * 0.35 * ahead, ahead }
  end
  if #values == 0 then return say(("the list has %d entity types"):format(#types)) end
  place(("entity types %d to %d of the list"):format(first, first + #values - 1), values, hostile and HOSTILE or FRIENDLY,
        places)
end

-- The layouts of the mapping probes: ring (creature-spawn), hostile (hostile-types), facing and ruler
-- (creature-units), and behaviors (entity-spawner-behaviors: one creature of each behaviour kind on an arc 4 blocks north of the player). Unlike the prototype's, they do not remove the previous layout first: the game API cannot set hp.
local function layout(verb, rest)
  local ht
  if verb == "hostile" then
    local word
    word, rest = rest:match("^(%S*)%s*(.-)$")
    ht = tonumber(word)
    if not ht then return say("usage: spawner hostile <hostile_type> [type]") end
  end
  local t, why = find_type(rest ~= "" and rest or verb == "facing" and "HUMAN_MALE" or "GREEN_SLIME")
  if not t then return say(why) end
  if verb == "behaviors" then
    for i, kind in ipairs(BEHAVIORS) do
      local angle = math.rad((i - 3.5) * 30)   -- an arc of 150 degrees, 4 blocks from the player, opening north
      place(("%s (%s)"):format(t.label, kind), t.value, hostility_for(kind),
            { { 4 * math.sin(angle), 4 * math.cos(angle) } }, kind)
    end
    return
  end
  if verb == "ring" then
    return place("a ring of four " .. t.label, t.value, hostile and HOSTILE or FRIENDLY,
                 { { 3, 0 }, { -3, 0 }, { 0, 3 }, { 0, -3 } })
  elseif verb == "hostile" then
    return place(("%s with hostile type %d, five 12 to 28 blocks north and one half a block east"):format(t.label, ht),
                 t.value, ht, { { 0, 28 }, { 0, 24 }, { 0, 20 }, { 0, 16 }, { 0, 12 }, { 0.5, 0 } })
  elseif verb == "facing" then
    return place(t.label .. " facing 0, 90, 180 and -90, 10 blocks north", t.value, FRIENDLY,
                 { { -6, 10, 0 }, { -2, 10, 90 }, { 2, 10, 180 }, { 6, 10, -90 } })
  end
  local places = {}
  for x = -4, 4, 2 do places[#places + 1] = { x, 7, 180 } end
  for y = 4, 16, 4 do places[#places + 1] = { 7, y, 180 } end
  place(t.label .. " in a row 7 blocks north and a line 7 blocks east", t.value, FRIENDLY, places)
end

-- ---------------------------------------------------------------- the window

local function set_open(value, why)
  if value == open then return end
  open = value
  note_listed = open
  kyubu.info(("window %s (%s)"):format(open and "opened" or "closed", why))
end

local function matches(t, f)
  return f == "" or t.readable:lower():find(f, 1, true) ~= nil or t.name:lower():find(f, 1, true) ~= nil
end

kyubu.on("render.overlay", function()
  if not open then return end
  local visible, still_open = kyubu.ui.begin("Entity spawner", true)
  if visible then
    local types, why = entity_types()
    local available, not_here = kyubu.game.available("spawn")
    if not types or not available then
      kyubu.ui.text("Spawning is not available: " .. tostring(why or not_here))
    else
      local changed, text = kyubu.ui.input_text("filter", filter, 64)
      if changed then filter, note_listed = text, true end
      local _, lv = kyubu.ui.slider_float("level (0: yours)", level, 0, 100)
      level = math.floor(lv + 0.5)
      kyubu.ui.text("behavior:")
      for _, b in ipairs(BEHAVIORS) do
        kyubu.ui.same_line()
        if kyubu.ui.button(b == behavior and "[" .. b .. "]" or b) then behavior = b end
      end
      if chooses_hostility(behavior) then
        local _, h = kyubu.ui.checkbox("hostile (off: on the player's side, the player cannot hurt it)", hostile)
        hostile = h
      end
      if chooses_name(behavior) then
        local _, n = kyubu.ui.checkbox("named (it has a name and can be talked to; off: nameless)", named)
        named = n
      end
      kyubu.ui.text(status)
      kyubu.ui.separator()
      local low, listed = filter:lower(), {}
      for _, t in ipairs(types) do
        if matches(t, low) then
          if #listed % COLUMNS ~= 0 then kyubu.ui.same_line() end
          listed[#listed + 1] = t.label
          if kyubu.ui.button(t.label) then spawn(t, 1) end
        end
      end
      if note_listed then
        note_listed = false
        kyubu.info(("the window lists %d of %d entity types (filter \"%s\")%s"):format(#listed, #types, filter,
          #listed <= 10 and ": " .. table.concat(listed, ", ") or ""))
      end
    end
  end
  kyubu.ui.finish()
  if not still_open then set_open(false, "its close button") end
end)

-- In a world the game puts the pointer back in the middle of the screen every frame (the mouse turns the camera), so
-- while the window is open this skips that and the pointer can reach the buttons, as the loader does for its console.
-- The pointer shows once it is over the window.
local hooked, hook_why = kyubu.hook_import("USER32.dll", "SetCursorPos", "int(__stdcall*)(int, int)",
  function(next, x, y)
    if open then return 1 end
    return next(x, y)
  end)
if not hooked then kyubu.warn("the pointer stays in the middle while the window is open: " .. tostring(hook_why)) end

-- The toggle key: "F1" to "F24", a letter or a digit, as a Windows virtual-key code.
local function key_code(name)
  name = tostring(name or ""):upper()
  local f = tonumber(name:match("^F(%d+)$"))
  if f and f >= 1 and f <= 24 then return 0x6F + f end
  if name:match("^[A-Z0-9]$") then return name:byte() end
end

local toggle_vk
local function read_key()
  local name = kyubu.config.get("toggle_key")
  toggle_vk = key_code(name)
  if not toggle_vk then
    kyubu.warn(("toggle_key %q is not F1 to F24, a letter or a digit: F7 opens the window"):format(tostring(name)))
    toggle_vk = 0x76
  end
end
read_key()

kyubu.on("input.key", function(ev)
  if ev.vk == toggle_vk and ev.down and not ev.is_repeat then
    set_open(not open, "its key")
    ev:cancel()
  end
end)

kyubu.on("config.changed", function(ev)
  if ev.mod ~= kyubu.id then return end
  if ev.key == "toggle_key" then read_key()
  elseif ev.key == "level" then level = tonumber(kyubu.config.get("level")) or level
  elseif ev.key == "hostile" then hostile = kyubu.config.get("hostile") ~= false
  elseif ev.key == "named" then named = kyubu.config.get("named") == true
  elseif ev.key == "behavior" or ev.key == "fights" then behavior = configured_behavior() end
end)

-- The list is read once a world is loaded (the game API offers it any time; this only logs it for the replays).
kyubu.on("lifecycle.world_enter", function()
  local types, why = entity_types()
  kyubu.info(types and ("%d entity types to spawn"):format(#types) or ("no entity types: " .. tostring(why)))
end)

-- ---------------------------------------------------------------- the console command

-- `where [seconds]` (probe creature-get-player) logs the player's state now and again after that many seconds.
local later = {}   -- the delayed reads: { at = os.time() when due, seconds = the delay }

local function where(print, note)
  local p, why = kyubu.game.creature_player()
  local info
  if p then info, why = p:get() end
  if not info then
    kyubu.warn("where: no player: " .. tostring(why))
    return print("no player: " .. tostring(why))
  end
  local line = ("where%s: %s type %d hostile %d level %d hp %.1f at %.0f %.0f %.0f yaw %.2f"):format(note or "",
    tostring(p), info.type, info.hostile, info.level, info.hp, info.pos.x, info.pos.y, info.pos.z, info.yaw)
  kyubu.info(line)
  print(line)
end

kyubu.on("frame.begin", function()
  local due = later[1]
  if due and os.time() >= due.at then
    table.remove(later, 1)
    where(function() end, (" (%d s later)"):format(due.seconds))
  end
end)

kyubu.command("entity_spawner:spawner", USAGE:sub(8) .. ": no argument opens or closes the window", function(args, print)
  args = args:gsub("^%s+", ""):gsub("%s+$", "")
  local verb, rest = args:match("^(%S*)%s*(.-)$")
  if args == "" then
    set_open(not open, "the console")
    return print("window " .. (open and "opened" or "closed"))
  elseif verb == "all" then
    all(rest)
  elseif verb == "list" then
    local types, why = entity_types()
    if not types then return print("no entity types: " .. tostring(why)) end
    local f, names = rest:lower(), {}
    for _, t in ipairs(types) do
      if matches(t, f) then names[#names + 1] = ("%s=%d"):format(t.name, t.value) end
    end
    return print(#names .. " types: " .. table.concat(names, " "))
  elseif verb == "where" then
    where(print)
    local seconds = tonumber(rest)
    if seconds and seconds > 0 then later[#later + 1] = { at = os.time() + seconds, seconds = seconds } end
    return
  elseif verb == "ring" or verb == "hostile" or verb == "facing" or verb == "ruler" or verb == "behaviors" then
    layout(verb, rest)
  else
    local t, why = find_type(verb)
    if not t then return print(why .. "; " .. USAGE) end
    -- what follows the type: a count, a behaviour and `named` or `unnamed`, each optional, in any order
    local count, kind, counted, is_named = 1, nil, false, nil
    for word in rest:gmatch("%S+") do
      local n = tonumber(word)
      local w = word:lower()
      if is_behavior(w) and not kind then kind = w
      elseif (w == "named" or w == "unnamed") and is_named == nil then is_named = w == "named"
      elseif n and not counted then count, counted = n, true
      else return print(("not a count, a behavior (%s) or named/unnamed: %s"):format(table.concat(BEHAVIORS, ", "), word)) end
    end
    if count < 1 or count > MAX_COUNT or count % 1 ~= 0 then
      return print(("the count is a whole number from 1 to %d"):format(MAX_COUNT))
    end
    spawn(t, count, kind, is_named)
  end
  print(status)
end)
