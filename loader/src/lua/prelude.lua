-- Kyubu Lua prelude: runs once per VM before any mod script. `K` is the table of C primitives.
local K = ...
local ffi_ok, ffi = pcall(require, "ffi")
local traceback = debug.traceback

if ffi_ok then
  ffi.cdef[[
    typedef struct KyubuCpuContext {
      uint32_t edi, esi, ebp, esp_unused, ebx, edx, ecx, eax;
      uint32_t eflags, esp, eip;
      union { float f32[4]; uint32_t u32[4]; } xmm[8];
    } KyubuCpuContext;
  ]]
end

-- ------------------------------------------------------------------ value packing for channels
local function pack(v, out)
  local t = type(v)
  if t == "nil" then out[#out + 1] = "n"
  elseif t == "boolean" then out[#out + 1] = v and "T" or "F"
  elseif t == "number" then local s = string.format("%.17g", v); out[#out + 1] = "d" .. #s .. ":" .. s
  elseif t == "string" then out[#out + 1] = "s" .. #v .. ":" .. v
  elseif t == "table" then
    out[#out + 1] = "{"
    for k, x in pairs(v) do pack(k, out); pack(x, out) end
    out[#out + 1] = "}"
  else error("cannot pack a " .. t) end
end

local function unpack_at(s, i)
  local c = s:sub(i, i)
  if c == "n" then return nil, i + 1
  elseif c == "T" then return true, i + 1
  elseif c == "F" then return false, i + 1
  elseif c == "d" or c == "s" then
    local colon = s:find(":", i, true)
    local n = tonumber(s:sub(i + 1, colon - 1))
    local body = s:sub(colon + 1, colon + n)
    return c == "d" and tonumber(body) or body, colon + n + 1
  elseif c == "{" then
    local t = {}
    i = i + 1
    while s:sub(i, i) ~= "}" do
      local k, v
      k, i = unpack_at(s, i)
      v, i = unpack_at(s, i)
      t[k] = v
    end
    return t, i + 1
  end
  error("malformed packed data at " .. i)
end

local MAGIC = "\0KT"

local function make_api(handle, mod_id, side, trust)
  local api = {}
  local full = trust == "full"
  local keep = {}          -- keeps FFI callbacks and next-cells alive
  local unload = {}

  api.id, api.side, api.trust = mod_id, side, trust
  api.pack = function(v) local o = {}; pack(v, o); return MAGIC .. table.concat(o) end
  api.unpack = function(s)
    if type(s) ~= "string" or s:sub(1, 3) ~= MAGIC then return s end
    return (unpack_at(s, 4))
  end

  local function log(level, ...)
    local parts = {}
    for i = 1, select("#", ...) do parts[#parts + 1] = tostring((select(i, ...))) end
    K.log(handle, level, table.concat(parts, " "))
  end
  api.log = function(...) log(1, ...) end
  api.debug = function(...) log(0, ...) end
  api.info = function(...) log(1, ...) end
  api.warn = function(...) log(2, ...) end
  api.error = function(...) log(3, ...) end

  api.phase = K.phase
  api.host = K.host
  api.current_side = K.current_side
  api.game_dir = K.game_dir
  api.data_dir = function() return K.data_dir(handle) end
  api.read_file = function(path) return K.read_package_file(handle, path) end
  api.is_mod_loaded = K.is_mod_loaded
  api.manifest = K.manifest
  api.mods = K.mods
  api.stats = K.stats
  api.mod_state = K.mod_state
  api.api_functions = K.api_functions
  api.run_on_main_thread = function(fn) return K.run_on_main_thread(handle, fn) end
  api.on_unload = function(fn) unload[#unload + 1] = fn end
  api._unload = function()
    for i = #unload, 1, -1 do
      local ok, err = xpcall(unload[i], traceback)
      if not ok then log(3, "on_unload handler failed: " .. tostring(err)) end
    end
  end

  -- events
  api.on = function(name, fn, priority, which_side) return K.subscribe(handle, name, fn, priority or 0, which_side) end
  api.off = function(id) return K.unsubscribe(handle, id) end
  api.emit = function(name, value, cancellable) return K.emit(handle, name, value, cancellable) end

  -- symbols & memory
  api.symbol = K.resolve_symbol
  api.rva_to_addr = K.rva_to_addr
  api.addr_to_rva = K.addr_to_rva
  api.read = K.mem_read

  -- registries
  api.registry = {
    create = function(name, schema, persistent) return K.registry_create(handle, name, schema or 1, persistent ~= false) end,
    add = function(reg, id, payload) return K.registry_add(handle, reg, id, payload) end,
    remap = function(reg, id, payload) return K.registry_remap(handle, reg, id, payload) end,
    lookup = K.registry_lookup, name = K.registry_name, payload = K.registry_payload,
    count = K.registry_count, entry = K.registry_entry, hash = K.registry_hash,
  }
  api.model = {
    add = function(id, key) return K.registry_add(handle, "kyubu:models", id, key) end,
    remap = function(id, key) return K.registry_remap(handle, "kyubu:models", id, key) end,
    slot = function(id) return K.registry_lookup("kyubu:models", id) end,
  }
  api.text = {
    add = function(id, en, de) return K.registry_add(handle, "kyubu:text", id, K.json_text(en, de or en)) end,
    get = K.text_get,
  }

  -- abilities (docs/abilities.md), full trust only (a restricted mod has no kyubu.ability). kyubu.ability.define(id, { name, description, icon, icon_file, duration_ms, cooldown_ms (a number or
  -- a function of the rank), mp_cost, max_rank, claim = { slot, class, specialization }, on_start, on_step, on_end,
  -- watch = { on_start, on_step, on_end } }) -> true. `watch` holds a joined client's view of another creature's run (ctx.side "watch").
  -- A cooldown function runs here once, for ranks 0 to max_rank. The callbacks go to the loader with the rest: the step runner calls
  -- them with a ctx table { creature, id, number, elapsed_ms, dt_ms, steps, rank, side, state, reason (on_end), cancel, marker }.
  api.ability = {
    define = function(id, def)
      if type(id) ~= "string" then error("kyubu.ability.define: the id must be a string", 2) end
      if type(def) ~= "table" then error("kyubu.ability.define: the definition must be a table", 2) end
      local flat = {}
      for _, k in ipairs({ "name", "description", "icon", "icon_file", "duration_ms", "mp_cost", "max_rank", "claim", "restarts" }) do flat[k] = def[k] end
      local ranks, cd = def.max_rank == nil and 1 or def.max_rank, def.cooldown_ms
      local ranks_ok = type(ranks) == "number" and ranks >= 1 and ranks <= 255 and ranks == math.floor(ranks)
      if type(cd) == "function" or type(cd) == "number" then
        if ranks_ok then   -- else the loader's check names max_rank
          local t = {}
          for r = 0, ranks do
            local v = cd
            if type(cd) == "function" then
              local ok, res = pcall(cd, r)
              if not ok then
                api.error("ability " .. id .. ": cooldown_ms failed for rank " .. r .. ": " .. tostring(res))
                return nil, "invalid argument"
              end
              v = res
            end
            if type(v) ~= "number" then error("kyubu.ability.define: 'cooldown_ms' must give whole numbers", 2) end
            t[r + 1] = v
          end
          flat.cooldown = t
        end
      else
        flat.cooldown = cd
      end
      for _, k in ipairs({ "on_start", "on_step", "on_end" }) do
        if def[k] ~= nil and type(def[k]) ~= "function" then error("kyubu.ability.define: '" .. k .. "' must be a function", 2) end
        flat[k] = def[k]
      end
      if def.watch ~= nil then
        if type(def.watch) ~= "table" then error("kyubu.ability.define: 'watch' must be a table", 2) end
        local w = {}
        for _, k in ipairs({ "on_start", "on_step", "on_end" }) do
          if def.watch[k] ~= nil and type(def.watch[k]) ~= "function" then error("kyubu.ability.define: 'watch." .. k .. "' must be a function", 2) end
          w[k] = def.watch[k]
        end
        flat.watch = w
      end
      if def.anim ~= nil and type(def.anim) ~= "string" then error("kyubu.ability.define: 'anim' must be a clip id (a string)", 2) end
      local ok, why = K.ability_define(handle, id, flat)
      if not ok or def.anim == nil then return ok, why end
      -- the clip binds after the definition: a clip that is not there fails here, and the ability stays defined without one
      return api.animation.bind(id, def.anim)
    end,
    -- number("demo:burst") -> 112, name(112) -> "demo:burst", list() -> { { id =, number = }, ... } in number order,
    -- slot_owner("warrior", 0) -> id, number (nil, "not found" when nobody holds the slot). A mod's ability has its number
    -- after the freeze; before it these give nil and the reason.
    number = K.ability_number, name = K.ability_name, list = K.ability_list, slot_owner = K.ability_slot_owner,
  }
  if not full then api.ability = nil end

  -- the animation binding (docs/game-api.md): a clip is data, so every trust level may bind its own ability's clip.
  -- kyubu.animation.bind("demo:slam", "demo:slam_clip") -> true, or nil and why; the `anim` key of kyubu.ability.define calls it.
  -- kyubu.animation.bind_for("demo:slam", creature_id, "demo:slam_end") binds a clip for one creature (its game id); nil drops it.
  api.animation = {
    bind = function(ability_id, clip_id) return K.ability_animation_bind(handle, ability_id, clip_id) end,
    bind_for = function(ability_id, creature_id, clip_id) return K.ability_animation_bind_for(handle, ability_id, creature_id, clip_id) end,
  }

  -- assets
  api.assets = {
    provider = function(db, glob, fn) return K.asset_provider(handle, db, glob, fn) end,
    read = K.asset_read,
    served_by = K.asset_served_by,
  }

  -- channels
  api.channel = {
    register = function(name, fn, which_side)
      return K.channel_register(handle, name, function(conn, data) return fn(conn, api.unpack(data)) end, which_side)
    end,
    send = function(name, conn, data)
      if type(data) == "table" then data = api.pack(data) end
      return K.channel_send(handle, name, conn, data or "")
    end,
    SERVER = 0, LOCAL_CLIENT = 1, ALL_CLIENTS = 0xFFFFFFFF,
  }

  -- interfaces
  api.publish = function(name, version, value) return K.publish_interface(handle, name, version, value) end
  api.interface = K.get_interface

  -- options (docs/config.md): only this mod's own, at every trust level
  api.config = {
    get = function(key) return K.config_get(handle, key) end,
    set = function(key, value) return K.config_set(handle, key, value) end,
  }

  -- console & UI
  api.command = function(name, help, fn) return K.register_command(handle, name, help or "", fn) end
  api.ui = K.ui

  -- full trust only: hooks, patches, raw memory writes
  local function need_full(what)
    if not full then error(what .. " is not available at trust level '" .. trust .. "'", 3) end
  end
  api.write = function(addr, bytes) need_full("kyubu.write"); return K.mem_write(handle, addr, bytes) end
  api.patch = function(addr, bytes) need_full("kyubu.patch"); return K.patch_bytes(handle, addr, bytes) end
  api.game_alloc = function(n) need_full("kyubu.game_alloc"); return K.game_alloc(n) end
  api.game_free = function(addr) need_full("kyubu.game_free"); return K.game_free(addr) end
  api.unhook = function(id) keep[id] = nil; return K.unhook(handle, id) end

  local function address_of(target)
    if type(target) == "string" then
      local a, err = K.resolve_symbol(target)
      if not a then error("symbol " .. target .. ": " .. tostring(err), 3) end
      return a
    end
    return target
  end

  local function make_hook(kind, ctype, fn, priority, a, b)
    need_full("kyubu.hook")
    local cell = ffi.new("void*[1]")
    local ct = ffi.typeof(ctype)
    local function nextfn(...) return ffi.cast(ct, cell[0])(...) end
    local cb = ffi.cast(ct, function(...)
      local ok, r = xpcall(fn, traceback, nextfn, ...)
      if not ok then
        K.hook_error(handle, tostring(r))
        return nextfn(...)
      end
      return r
    end)
    local id, err = K.hook_raw(handle, kind, tonumber(ffi.cast("uintptr_t", cb)), priority or 0,
                               tonumber(ffi.cast("uintptr_t", cell)), a, b)
    if not id then cb:free(); return nil, err end
    keep[id] = {cb, cell}
    return id
  end

  -- kyubu.hook(symbol_or_address, "int(__thiscall*)(void*, int)", function(next, self, x) return next(self, x) end, priority)
  api.hook = function(target, ctype, fn, priority) return make_hook("function", ctype, fn, priority, address_of(target)) end
  api.hook_import = function(dll, func, ctype, fn, priority) return make_hook("import", ctype, fn, priority, dll, func) end
  api.hook_vtable = function(object, index, ctype, fn, priority) return make_hook("vtable", ctype, fn, priority, object, index) end
  api.hook_mid = function(target, fn)
    need_full("kyubu.hook_mid")
    return K.hook_mid(handle, address_of(target), function(ctx) return fn(ffi.cast("KyubuCpuContext*", ctx)) end)
  end

  api.ffi = full and ffi or nil

  -- the game API (docs/game-api.md), full trust only for now
  if full then
    api.game = {
      available = K.game_available,   -- (primitive) -> true, or false and the reason
      enum = K.game_enum,             -- (name) -> { {value =, name =}, ... } in value order, or nil and the reason
      -- creatures are objects, one per handle per VM (c:valid(), c:get(), tostring(c)); failures give nil and the reason
      creature_get = function(c) return K.creature_get(handle, c) end,     -- (c) -> { id, type, hostile, level, hp, pos, yaw }
      creature_player = function() return K.creature_player(handle) end,  -- () -> the client side's own creature
      creature_find = function(id) return K.creature_find(handle, id) end, -- (id) -> the creature with that game id
      creature_spawn = function(t) return K.creature_spawn(handle, t) end, -- {type, hostile, level, pos, yaw} -> a new creature
      creature_teleport = function(c, x, y, z) return K.creature_teleport(handle, c, x, y, z) end, -- (c, x, y, z) -> true; whole units
      creature_set_yaw = function(c, yaw) return K.creature_set_yaw(handle, c, yaw) end, -- (c, degrees) -> true; counter-clockwise from north
      creature_set_flags = function(c, flags) return K.creature_set_flags(handle, c, flags) end, -- (c, {unstoppable =, invulnerable =, rooted =}) -> true; a missing key is false
      creature_state_get = function(c) return K.creature_state_get(handle, c) end, -- (c) -> {pos, velocity, knock, aim, mode, mode_ms, stun_ms, slowed_ms, charge}
      creature_state_set = function(c, t) return K.creature_state_set(handle, c, t) end, -- (c, the keys to write) -> true
      shot_take = function() return K.shot_take(handle) end,                            -- the same as shots
      shots = function() return K.shot_take(handle) end,                                -- () -> {{connection =, data = 0x70 bytes}, ...}; Server.exe
      projectile_push = function(d) return K.projectile_push(handle, d) end,            -- (0x70-byte cube::Projectile) -> true; Server.exe, in a step
      body_define = function(t) return K.body_define(handle, t) end, -- ({name, rig, pieces = {node = slot}, clips = {name = text}}) -> a body id, or nil and why
      body_name = function(b) return K.body_name(handle, b) end, -- (body) -> {pieces = {...}, clips = {...}}
      creature_body_attach = function(c, b, scale) return K.creature_body_attach(handle, c, b, scale) end, -- (c, body or nil, blocks a voxel) -> true
      creature_body_play = function(c, clip, o) return K.creature_body_play(handle, c, clip, o) end, -- (c, clip, {start =, loop =, fade =}) -> true
      creature_body_offset = function(c, part, ch, v, group) return K.creature_body_offset(handle, c, part, ch, v, group) end, -- (c, "head", "rot", {x, y, z}, group) -> true; (c) clears
      creature_body_weight = function(c, group, w) return K.creature_body_weight(handle, c, group, w) end, -- (c, group, weight) -> true
      creature_body_read = function(c) return K.creature_body_read(handle, c) end, -- (c) -> {clip, time, fade_left, loop, markers = {...}}
      creature_body_point = function(c, piece, p, clip, t) return K.creature_body_point(handle, c, piece, p, clip, t) end, -- (c, piece, {x, y, z}, clip, ms) -> x, y, z in blocks
      creature_give_item = function(c, item) return K.creature_give_item(handle, c, item) end, -- (c, {type, sub_type, level, rarity, material, modifier}) -> true
      ability_get = function(c) return K.ability_get(handle, c) end,        -- (c) -> { number, id, elapsed_ms }; number 0 is "in no ability" (also c:ability())
      ability_cooldown = function(c, a) return K.ability_cooldown(handle, c, a) end, -- (c, number or id) -> ms left, 0 for none (also c:cooldown(a))
      ability_cooldown_set = function(c, a, ms) return K.ability_cooldown_set(handle, c, a, ms) end, -- (c, number or id, ms) -> true; 0 gives it back, on Server.exe the admission too (also c:set_cooldown(a, ms))
      ability_start = function(c, a) return K.ability_start(handle, c, a) end,   -- (c, number or id) -> true, or nil and why it was refused
      ability_end = function(c) return K.ability_end(handle, c) end,        -- (c) -> true; the ability ends `cancelled`
      ability_rank_get = function(a) return K.ability_rank_get(handle, a) end,     -- (number or id) -> the local player's rank in a mod ability, or nil and why
      ability_rank_set = function(a, rank) return K.ability_rank_set(handle, a, rank) end, -- (number or id, rank) -> the rank stored (clamped to 0..max_rank), or nil and why
      ability_animation_bind = function(a, clip) return K.ability_animation_bind(handle, a, clip) end, -- (ability id, clip id) -> true, or nil and why (also kyubu.animation.bind, which every trust level has)
      ability_animation_bind_for = function(a, id, clip) return K.ability_animation_bind_for(handle, a, id, clip) end, -- (ability id, creature id, clip id or nil) -> true, or nil and why (also kyubu.animation.bind_for)
      -- the animation editor's calls: dev mode only, client side only
      animation_define_from_text = function(name, text) return K.animation_define_from_text(handle, name, text) end, -- (name, text) -> true, or nil and the errors
      animation_preview = function(clip, t_ms) return K.animation_preview(handle, clip, t_ms) end, -- (clip id, ms) -> true, or nil and why
      animation_preview_clear = function() return K.animation_preview_clear(handle) end,         -- () -> true
      -- the UI scene tree and the render device: client side, main thread, read only
      ui_node_find = function(name, nth) return K.ui_node_find(handle, name, nth) end,  -- (name, nth = 0) -> { node, parent, name, children, depth, colour }, or nil and why
      ui_node_get = function(node) return K.ui_node_get(handle, node) end,             -- (node address, nil: the root) -> the same table, or nil and why
      ui_node_child = function(node, index) return K.ui_node_child(handle, node, index) end, -- (node address, index) -> the child's table, or nil and why
      render_get = function() return K.render_get(handle) end,                        -- () -> { device = address, width =, height = }, or nil and why
      -- the game's own particle effects: client side
      particle_spawn = function(t) return K.particle_spawn(handle, t) end,           -- ({pos = {x, y, z}, type, count, scale, spreading, accel, color}) -> true, or nil and why
      -- point lights (models, particles and the ground): client side
      light_spawn = function(t) return K.light_spawn(handle, t) end,                 -- ({pos = {x, y, z}, radius, color = {r, g, b}, life_ms, fade_ms}) -> id, or nil and why
      light_set = function(id, t) return K.light_set(handle, id, t) end,             -- (id, the same table) -> true, or nil and why
      light_remove = function(id) return K.light_remove(handle, id) end,             -- (id) -> true, or nil and why
      world_block = function(x, y, z) return K.world_block(handle, x, y, z) end,    -- (x, y, z blocks) -> type, r, g, b; nil, "unknown" outside loaded zones
      -- a hit through the game's own hitCreature, inside a World step (sim.world_step): armor, the number, kills, XP, threat
      creature_hit = function(a, c, t) return K.creature_hit(handle, a, c, t) end, -- (attacker or nil, target, {damage, stun_ms, knock = {x, y, z}, magic}) -> landed
      hit = function(a, c, t) return K.creature_hit(handle, a, c, t) end,          -- the same: design D11's game.hit
    }
  end
  return api
end

return make_api
