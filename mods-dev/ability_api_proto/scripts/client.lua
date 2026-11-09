-- ability_api_proto, client side (task 6.1 of add-ability-api, probe ability-api). One ability on the warrior's slot 4 (800 ms,
-- mp_cost 0.3, cooldown 3500 ms at rank 1) and console commands that call the game-API ability functions on the player. A console
-- command takes about 15 s under the replay's software rendering, so what has to happen inside one ability run happens in its
-- callbacks: on_step reads ability_get while it runs (and, in "end" mode, calls ability_end); on_end reads the cooldown and tries
-- ability_start again. Every answer is one log line starting "ability_api_proto: ".
local ffi = require("ffi")
local ID = "ability_api_proto:burst"
local GC_PLAYER, P_MP, P_MODE, P_TIME = 0x8006C8, 0x170, 0x68, 0x6C
local run_mode = "plain"   -- "end": on_step 10 ends the ability through the API

local function u32(addr) return ffi.cast("uint32_t*", addr)[0] end
local function player_address()
  local g = kyubu.symbol("cube::game_controller")
  local gc = g and u32(g) or 0
  return gc ~= 0 and u32(gc + GC_PLAYER) or 0
end

local function describe(c)
  local a, why = c:ability()
  if not a then return "get failed: " .. tostring(why) end
  return ("number=%d id=%q elapsed_ms=%d"):format(a.number, a.id, a.elapsed_ms)
end

kyubu.on("lifecycle.register", function()
  if not kyubu.ability then kyubu.info("ability_api_proto: this loader has no kyubu.ability"); return end
  local ok, err = kyubu.ability.define(ID, {
    icon = "cubeworld:smash",
    duration_ms = 800,
    mp_cost = 0.3,
    max_rank = 3,
    cooldown_ms = function(rank) return 4000 - 500 * rank end,   -- rank 1: 3500
    claim = { slot = 4, class = "warrior", specialization = "any" },
    on_start = function(ctx) kyubu.info(("ability_api_proto: on_start %s number %d"):format(ctx.id, ctx.number)) end,
    on_step = function(ctx)
      local c = ctx.creature
      if ctx.steps == 5 then kyubu.info("ability_api_proto: while it runs, step 5: " .. describe(c)) end
      if ctx.steps == 10 and run_mode == "end" then
        kyubu.info("ability_api_proto: end at step 10 " .. tostring(select(2, kyubu.game.ability_end(c)) or "ok"))
      end
    end,
    on_end = function(ctx)
      local c = ctx.creature
      kyubu.info(("ability_api_proto: on_end %s after %d steps, %d ms"):format(ctx.reason, ctx.steps, ctx.elapsed_ms))
      kyubu.info(("ability_api_proto: in on_end: cooldown burst=%s, %s"):format(tostring(c:cooldown(ID)), describe(c)))
      local ok2, why = kyubu.game.ability_start(c, ID)
      kyubu.info(("ability_api_proto: start again in on_end: %s"):format(ok2 and "ok" or ("refused: " .. tostring(why))))
    end,
  })
  kyubu.info(("ability_api_proto: define %s: %s"):format(ID, ok and "ok" or tostring(err)))
end)

local function say(print, text) kyubu.info(text); print(text) end
local function player() return kyubu.game and kyubu.game.creature_player() end
local function answer(ok, why) return ok and "ok" or ("refused: " .. tostring(why)) end

local function set_mp(x)
  local p = player_address()
  if p ~= 0 then ffi.cast("float*", p + P_MP)[0] = x end
end

kyubu.command("ability_api_proto:get", "ability_api_proto:get: ability_get on the player", function(_, print)
  local p = player()
  if not p then say(print, "ability_api_proto: no player"); return end
  say(print, "ability_api_proto: get " .. describe(p))
end)

kyubu.command("ability_api_proto:cd", "ability_api_proto:cd: ability_cooldown of the burst, smash and the placeholder", function(_, print)
  local p = player()
  if not p then say(print, "ability_api_proto: no player"); return end
  local nothing, why = p:cooldown("no:such")
  say(print, ("ability_api_proto: cooldown burst=%s smash=%s placeholder=%s unknown=%s/%s"):format(
    tostring(p:cooldown(ID)), tostring(kyubu.game.ability_cooldown(p, 54)), tostring(p:cooldown("kyubu:placeholder")), tostring(nothing), tostring(why)))
end)

-- start, with the mp set first (a warrior's mp is rage: 0 unless a hit earned it); `end` mode: ends itself at step 10
local function start(print, mode, mp)
  local p = player()
  if not p then say(print, "ability_api_proto: no player"); return end
  run_mode = mode
  set_mp(mp)
  say(print, ("ability_api_proto: start burst (mp %.1f, %s mode) %s"):format(mp, mode, answer(kyubu.game.ability_start(p, ID))))
end
kyubu.command("ability_api_proto:start", "ability_api_proto:start: mp 1, then ability_start of the burst", function(_, print) start(print, "plain", 1) end)
kyubu.command("ability_api_proto:startend", "ability_api_proto:startend: as start, and the burst ends itself through ability_end", function(_, print) start(print, "end", 1) end)
kyubu.command("ability_api_proto:startlow", "ability_api_proto:startlow: mp 0.1, then ability_start (refused)", function(_, print) start(print, "plain", 0.1) end)

kyubu.command("ability_api_proto:vanilla", "ability_api_proto:vanilla: ability_start of smash (a vanilla ability: refused)", function(_, print)
  local p = player()
  if not p then say(print, "ability_api_proto: no player"); return end
  say(print, "ability_api_proto: start smash " .. answer(kyubu.game.ability_start(p, 54)))
end)

kyubu.command("ability_api_proto:end", "ability_api_proto:end: ability_end on the player", function(_, print)
  local p = player()
  if not p then say(print, "ability_api_proto: no player"); return end
  say(print, "ability_api_proto: end " .. answer(kyubu.game.ability_end(p)))
end)

-- the game's smash forced on the player (a level-1 warrior has not learned it), read at once
kyubu.command("ability_api_proto:smash", "ability_api_proto:smash: forces mode 54, then ability_get", function(_, print)
  local p, a = player(), player_address()
  if not p or a == 0 then say(print, "ability_api_proto: no player"); return end
  ffi.cast("uint8_t*", a + P_MODE)[0] = 54
  ffi.cast("int32_t*", a + P_TIME)[0] = 0
  say(print, "ability_api_proto: forced smash: " .. describe(p))
end)

-- task 5.2: the player's rank in the burst (ability_rank_get / ability_rank_set); a rank set here is written to the ranks file at once
kyubu.command("ability_api_proto:rankget", "ability_api_proto:rankget: ability_rank_get of the burst", function(_, print)
  local rank, why = kyubu.game.ability_rank_get(ID)
  say(print, "ability_api_proto: rank get " .. (rank and ("rank " .. rank) or ("refused: " .. tostring(why))))
end)
for _, n in ipairs({ 0, 1, 2, 9 }) do
  kyubu.command("ability_api_proto:rankset" .. n, "ability_api_proto:rankset" .. n .. ": ability_rank_set of the burst to " .. n, function(_, print)
    local stored, why = kyubu.game.ability_rank_set(ID, n)
    say(print, ("ability_api_proto: rank set %d: %s"):format(n, stored and ("stored " .. stored) or ("refused: " .. tostring(why))))
  end)
end
