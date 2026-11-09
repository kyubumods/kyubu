-- Lua API parity checklist. The list of native functions comes from the loader itself
-- (kyubu.api_functions(), generated from the side-rule table), so a new native function without a
-- Lua check shows up here as "no Lua check".
local S = kyubu.side
local ffi = kyubu.ffi
local R = {}                                   -- native name -> true | failure text
local EXEMPT = {
  imgui_context = "Lua scripts draw through kyubu.ui instead of a raw ImGuiContext",
}
local function check(name, fn)
  local ok, v, e = pcall(fn)
  if ok and v then R[name] = true
  else R[name] = ok and ("returned " .. tostring(v) .. (e and (" " .. tostring(e)) or "")) or tostring(v) end
end
local function also(name, other) R[name] = R[other] end
local REG = "fx_parity:things_" .. S

-- ---------------------------------------------------------------- PreInit
check("registry_create", function() return kyubu.registry.create(REG, 1, false) end)
-- a valid 1x1x1 .cub model (sizes, then one RGB voxel)
local provided = string.char(1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 200, S == "client" and 10 or 20, 30)
check("asset_provider", function()
  return kyubu.assets.provider("data1.db", "fx-parity-" .. S .. ".cub", function(db, key) return provided end)
end)
kyubu.on("lifecycle.register", function()
  check("registry_add", function() return kyubu.registry.add(REG, "fx_parity:a", "one") end)
  check("registry_remap", function() return kyubu.registry.remap(REG, "fx_parity:a", "two") end)
  kyubu.text.add("fx_parity:greeting", "Hello", "Hallo")
  -- the same data from both sides is allowed
  check("ability_define", function() return kyubu.ability.define("fx_parity:burst", { duration_ms = 500, cooldown_ms = 1000, max_rank = 2 }) end)
end)

-- ---------------------------------------------------------------- GameReady
local keep = {}
local function addr(p) return tonumber(ffi.cast("uintptr_t", p)) end

local function run_checks()
  check("log", function() kyubu.info("parity: log works"); return true end)
  check("phase", function() return kyubu.phase() == "game_ready" end)
  check("current_side", function() return kyubu.current_side() == S end)
  check("host_info", function() local h = kyubu.host(); return h.build ~= nil and h.host ~= nil end)
  check("mod_id", function() return kyubu.id == "fx_parity" end)
  check("game_dir", function() return type(kyubu.game_dir()) == "string" end)
  check("mod_data_dir", function() return type(kyubu.data_dir()) == "string" end)
  check("read_package_file", function() return (kyubu.read_file("mod.toml") or ""):find("fx_parity", 1, true) end)
  check("is_mod_loaded", function() return kyubu.is_mod_loaded("fx_parity") == "1.0.0" end)
  -- providers return Lua strings; the loader copies them with buffer_alloc and the overlay frees them
  check("buffer_alloc", function()
    local data, layer = kyubu.assets.read("data1.db", "fx-parity-" .. S .. ".cub")
    return data == provided and layer == "provider:fx_parity"
  end)
  also("buffer_free", "buffer_alloc")
  check("run_on_main_thread", function() return kyubu.run_on_main_thread(function() end) end)

  -- symbols & memory
  local dbget = kyubu.symbol("Database::get")
  check("resolve_symbol", function() return dbget ~= nil end)
  check("rva_to_addr", function() return kyubu.addr_to_rva(kyubu.rva_to_addr(0x1000)) == 0x1000 end)
  also("addr_to_rva", "rva_to_addr")
  local buf = ffi.new("uint8_t[16]"); keep[#keep + 1] = buf
  check("mem_write", function() return kyubu.write(addr(buf), "\1\2\3") end)
  check("mem_read", function() return kyubu.read(addr(buf), 3) == "\1\2\3" end)
  check("game_alloc", function()
    local p = kyubu.game_alloc(64)
    if not p then return false end
    kyubu.write(p, "abc")
    local ok = kyubu.read(p, 3) == "abc"
    kyubu.game_free(p)
    return ok
  end)
  also("game_free", "game_alloc")

  -- hooks & patches
  local sig = "bool(__thiscall*)(void*, void*, void**, uint32_t*)"
  check("hook_symbol", function()
    local id, err = kyubu.hook("Database::get", sig, function(next, ...) return next(...) end)
    return id and kyubu.unhook(id), err
  end)
  check("hook_function", function()
    local id, err = kyubu.hook(dbget, sig, function(next, ...) return next(...) end)
    return id and kyubu.unhook(id), err
  end)
  also("unhook", "hook_function")
  check("hook_mid", function()
    local id, err = kyubu.hook_mid("ModelLoader::resizeSite", function(ctx) end)
    return id and kyubu.unhook(id), err
  end)
  check("hook_import", function()
    local calls = 0
    local id, err = kyubu.hook_import("kernel32.dll", "GetTickCount", "uint32_t(__stdcall*)()",
      function(next) calls = calls + 1; return next() end)
    return id and kyubu.unhook(id), err
  end)
  check("hook_vtable", function()
    local ct = "int(__thiscall*)(void*, int)"
    local method = ffi.cast(ct, function(self, x) return x + 1 end); keep[#keep + 1] = method
    local vtbl = ffi.new("void*[1]"); vtbl[0] = ffi.cast("void*", method)
    local obj = ffi.new("void*[1]"); obj[0] = ffi.cast("void*", vtbl)
    keep[#keep + 1] = vtbl; keep[#keep + 1] = obj
    local id, err = kyubu.hook_vtable(addr(obj), 0, ct, function(next, self, x) return next(self, x) * 10 end)
    if not id then return false, err end
    local r = ffi.cast(ct, vtbl[0])(obj, 1)
    kyubu.unhook(id)
    local after = ffi.cast(ct, vtbl[0])(obj, 1)
    return r == 20 and after == 2, ("r=%s after=%s"):format(r, after)
  end)
  check("patch_bytes", function()
    local p = ffi.new("uint8_t[8]"); keep[#keep + 1] = p
    local id, err = kyubu.patch(addr(p), "\144\144")
    if not id then return false, err end
    local patched = p[0] == 0x90 and p[1] == 0x90
    kyubu.unhook(id)
    return patched and p[0] == 0
  end)

  -- events
  local seen = false
  local sub = kyubu.on("fx_parity:ping", function(ev) seen = true; ev:cancel() end)
  check("subscribe", function() return sub ~= nil end)
  check("emit", function() return kyubu.emit("fx_parity:ping", nil, true) == true and seen end)
  also("event_cancel", "emit")
  check("unsubscribe", function() return kyubu.off(sub) and kyubu.emit("fx_parity:ping", nil, true) == false end)

  -- assets
  check("asset_read", function() return #(kyubu.assets.read("data1.db", "iron-sword1.cub") or "") > 0 end)
  check("asset_served_by", function() return kyubu.assets.served_by("data1.db", "fx-parity-" .. S .. ".cub") == "provider:fx_parity" end)

  -- registries
  local n = kyubu.registry.lookup(REG, "fx_parity:a")
  check("registry_lookup", function() return n ~= nil end)
  check("registry_name", function() return kyubu.registry.name(REG, n) == "fx_parity:a" end)
  check("registry_payload", function() return kyubu.registry.payload(REG, "fx_parity:a") == "two" end)
  check("registry_count", function() return kyubu.registry.count(REG) == 1 end)
  check("registry_entry", function() local id, num = kyubu.registry.entry(REG, 0); return id == "fx_parity:a" and num == n end)
  check("registry_hash", function() return #(kyubu.registry.hash(REG) or "") == 64 end)

  -- abilities: a mod's number exists after the freeze, and the slot table is built right after it
  local burst = kyubu.ability.number("fx_parity:burst")
  check("ability_number", function() return burst ~= nil and burst >= 112 and kyubu.ability.number("cubeworld:smash") == 54 end)
  check("ability_name", function() return kyubu.ability.name(burst) == "fx_parity:burst" end)
  check("ability_list", function()
    local l = kyubu.ability.list()
    return l ~= nil and #l >= 113 and l[1].number == 0 and l[burst + 1].id == "fx_parity:burst"
  end)
  check("ability_slot_owner", function()
    local id, num = kyubu.ability.slot_owner("warrior", 0)
    local _, why = kyubu.ability.slot_owner("paladin", 0)
    return type(id) == "string" and type(num) == "number" and why == "invalid argument"
  end)

  -- channels & session
  check("channel_register", function() return kyubu.channel.register("fx_parity:echo", function(conn, data) end) end)
  check("channel_send", function()
    local to = S == "client" and kyubu.channel.SERVER or kyubu.channel.LOCAL_CLIENT
    return kyubu.channel.send("fx_parity:echo", to, "hi")
  end)
  check("session_manifest", function() return (kyubu.manifest() or ""):find("fx_parity", 1, true) end)

  -- interfaces
  local iface = ffi.new("int[1]", 7); keep[#keep + 1] = iface
  check("publish_interface", function() return kyubu.publish("fx_parity:iface_" .. S, 2, addr(iface)) end)
  check("get_interface", function()
    local p, v = kyubu.interface("fx_parity:iface_" .. S, 1)
    return p and v == 2 and ffi.cast("int*", p)[0] == 7
  end)

  -- diagnostics
  check("mod_stats", function() return type(kyubu.stats("fx_parity")) == "number" end)
  check("mod_state", function() return kyubu.mod_state("fx_parity") == "loaded" end)

  -- game API: a known primitive answers true, or false with the reason; an unknown one false with a reason
  check("game_available", function()
    local ok, why = kyubu.game.available("world")
    local unknown, unknown_why = kyubu.game.available("fx_parity_no_such_primitive")
    return (ok == true or type(why) == "string") and unknown == false and type(unknown_why) == "string"
  end)
  -- a verified enum lists its values in value order; an unknown one answers nil and a reason
  check("game_enum", function()
    local types = kyubu.game.enum("cube::EntityType")
    local unknown, unknown_why = kyubu.game.enum("fx_parity::NoSuchEnum")
    return types and #types > 0 and types[1].value == 0 and type(types[1].name) == "string"
      and unknown == nil and type(unknown_why) == "string"
  end)
  -- creatures: the client's player (the same object twice), or nil and the reason when no world is loaded; reading it
  -- gives its position, and anything but a creature object is an error
  if S == "client" then
    check("creature_player", function()
      local p, why = kyubu.game.creature_player()
      if p then return p == kyubu.game.creature_player() and p:valid() end
      return p == nil and type(why) == "string"
    end)
  end
  check("creature_get", function()
    local p = S == "client" and kyubu.game.creature_player()
    local bad = pcall(kyubu.game.creature_get, 42) == false
    if not p then return bad end
    local info = kyubu.game.creature_get(p)
    return bad and info ~= nil and type(info.pos.x) == "number" and info.id == p:get().id
  end)
  -- spawning is the server side's (at game_ready no world is loaded: nil and the reason); a field of the wrong type
  -- is an error
  if S == "server" then
    check("creature_spawn", function()
      local bad = pcall(kyubu.game.creature_spawn, { type = "slime" }) == false
      local c, why = kyubu.game.creature_spawn{ type = 37, hostile = 1, level = 1, pos = {0, 0, 0} }
      if c then return bad and c:valid() end
      return bad and c == nil and type(why) == "string"
    end)
  end

  -- find, teleport and give_item need a world: here a wrong argument is an error, and a good call without a world gives
  -- nil and the reason (a creature that is not one, a fractional id, a missing item type)
  check("creature_find", function()
    local bad = pcall(kyubu.game.creature_find, 1.5) == false
    local c, why = kyubu.game.creature_find(1)
    if c then return bad and c:valid() end
    return bad and c == nil and type(why) == "string"
  end)
  check("creature_teleport", function()
    return pcall(kyubu.game.creature_teleport, 42, 0, 0, 0) == false
  end)
  if S == "client" then
    check("creature_give_item", function()
      return pcall(kyubu.game.creature_give_item, 42, { type = 1 }) == false
    end)
    check("particle_spawn", function()
      -- a wrong argument raises; a good table without a world gives nil and the reason
      local bad = pcall(kyubu.game.particle_spawn, 42) == false and pcall(kyubu.game.particle_spawn, {}) == false
      local ok, why = kyubu.game.particle_spawn({ pos = { 0, 0, 0 } })
      return bad and (ok == true or (ok == nil and type(why) == "string"))
    end)
  end

  -- the clip binding: the fixture ships no clip, so a good call fails with the clip named; a vanilla ability is refused first on the client side
  -- only if the provider is verified (else the unavailable reason), so the check is that both Lua names reach the native call and give a reason
  check("ability_animation_bind", function()
    local ok, why = kyubu.animation.bind("fx_parity:burst", "fx_parity:nothing")
    local ok2, why2 = kyubu.game.ability_animation_bind("cubeworld:smash", "fx_parity:nothing")
    return ok == nil and type(why) == "string" and ok2 == nil and type(why2) == "string"
  end)

  -- options (config.toml): one getter per type, and a set that is saved and read back
  check("config_get_bool", function() return kyubu.config.get("flag") == true end)
  check("config_get_int", function() return kyubu.config.get("count") == 3 end)
  check("config_get_float", function() return kyubu.config.get("scale") == 1.5 end)
  check("config_get_string", function() return kyubu.config.get("name") == "fx" end)
  check("config_set", function()
    local bad = kyubu.config.set("count", 99)   -- above max: refused
    return bad == nil and kyubu.config.set("count", 4) and kyubu.config.get("count") == 4
      and kyubu.config.set("count", 3)
  end)
end

-- client-only functions: exercised from render.overlay on the client, refused on the server
local function ui_checks()
  local ui = kyubu.ui
  check("register_command", function() return kyubu.command("fx_parity:cmd", "parity", function() end) end)
  check("ui_begin", function()
    local visible = ui.begin("parity")
    ui.text("parity"); ui.same_line(); ui.separator()
    ui.button("b"); ui.checkbox("c", false); ui.slider_float("s", 0.5, 0, 1); ui.input_text("t", "x", 16)
    ui["end"]()
    return type(visible) == "boolean"
  end)
  for _, f in ipairs({"ui_end", "ui_text", "ui_button", "ui_checkbox", "ui_slider_float", "ui_input_text",
                      "ui_same_line", "ui_separator"}) do also(f, "ui_begin") end
  check("ui_want_input", function() return type(ui.want_input()) == "boolean" end)
end

local function refused_on_server()
  local ui, ok, total = kyubu.ui, 0, 0
  local function expect(cond) total = total + 1; if cond then ok = ok + 1 end end
  expect(kyubu.command("fx_parity:srv", "", function() end) == nil)
  expect(ui.begin("x") == false)
  expect(ui.button("x") == false)
  expect(ui.checkbox("x", true) == false)
  expect(ui.want_input() == false)
  kyubu.info(("side rules: %d/%d client-only calls refused on the server side"):format(ok, total))
end

local function report()
  local native = kyubu.api_functions()
  local total, good, missing, failed, exempt = 0, 0, {}, {}, {}
  for name, side in pairs(native) do
    if side == "both" or side == S then
      total = total + 1
      local r = R[name]
      if EXEMPT[name] then exempt[#exempt + 1] = name .. " (" .. EXEMPT[name] .. ")"; good = good + 1
      elseif r == true then good = good + 1
      elseif r == nil then missing[#missing + 1] = name
      else failed[#failed + 1] = name .. ": " .. r end
    end
  end
  table.sort(missing); table.sort(failed)
  kyubu.info(("API parity: %d/%d native functions reachable from Lua on the %s side"):format(good, total, S))
  if #exempt > 0 then kyubu.info("exempt: " .. table.concat(exempt, ", ")) end
  if #missing > 0 then kyubu.warn("no Lua check for: " .. table.concat(missing, ", ")) end
  if #failed > 0 then kyubu.warn("failed: " .. table.concat(failed, "; ")) end
end

-- text registry: current-language lookup, explicit languages, and the merged game dictionaries
local function text_check()
  local en = kyubu.assets.read("data4.db", "dict_en.xml") or ""
  local de = kyubu.assets.read("data4.db", "dict_de.xml") or ""
  local in_dict = en:find('key="fx_parity:greeting"', 1, true) and en:find("Hello", 1, true)
                  and de:find('key="fx_parity:greeting"', 1, true) and de:find("Hallo", 1, true)
  kyubu.info(("text: current=%s en=%s de=%s dictionaries=%s"):format(tostring(kyubu.text.get("fx_parity:greeting")),
    tostring(kyubu.text.get("fx_parity:greeting", "en")), tostring(kyubu.text.get("fx_parity:greeting", "de")),
    in_dict and "ok" or "missing"))
end

local checks_done = false
kyubu.on("lifecycle.game_ready", function()
  run_checks()
  if S == "client" then text_check() end
  checks_done = true
  if S == "server" then refused_on_server(); report() end
end)
if S == "client" then
  local reported = false
  kyubu.on("render.overlay", function()
    if checks_done and not reported then reported = true; ui_checks(); report() end
  end)
end
