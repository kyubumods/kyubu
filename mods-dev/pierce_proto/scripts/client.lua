-- pierce_proto, client side (task 8.2c of add-skills-unleashed, probe projectile-pierce).
-- A ranger of specialization 1 is "armed":
--   * the M1 key byte (GameController+4) is copied onto the M2 byte (+5) at the bar-built site, so M1 holds the charge (mode 25);
--   * the release (the game writes mode 55, a volley of four arrows) is changed to mode 23, one arrow, at 0x49C069;
--   * that arrow (the block of modes 22, 23, 26) gets kind 1 and skill 0 and 1.5 times the speed at 0x615E03, before it is pushed:
--     kind 1 flies straight and, without the skill flag, hits through hitCreature and stops at the first creature;
--   * the stop is cleared after each hit (0x62076E), and hitCreature is skipped for a creature this arrow has hit already;
--   * at a surface the kind is put back to 0 (0x62094E), so the game ends it as it ends its own arrow: a puff, no explosion.
-- The first three creatures that are not the player are put at 8, 16 and 24 blocks in front of the player (+y), still and hostile,
-- for any ranger (specialization 0 shows what the game's own bow does to them).
local ffi = require("ffi")

local GC_PLAYER, GC_WORLD = 0x8006C8, 0x2DC
local P_ID, P_POS, P_HOSTILE, P_MODE, P_TIME, P_MP = 0x08, 0x10, 0x60, 0x68, 0x6C, 0x170
local P_CLASS, P_SPEC, P_STUN, P_HP, P_HPMULT = 0x140, 0x141, 0x12C, 0x16C, 0x178
local ARMED_CLASS, ARMED_SPEC = 2, 1
local BLOCK = 65536

local function u8(a) return ffi.cast("uint8_t*", a)[0] end
local function u32(a) return ffi.cast("uint32_t*", a)[0] end
local function i32(a) return ffi.cast("int32_t*", a)[0] end
local function f32(a) return ffi.cast("float*", a)[0] end
local function i64(a) return tonumber(ffi.cast("int64_t*", a)[0]) end

local function controller()
  local p = kyubu.symbol("cube::game_controller")
  return p and u32(p) or 0
end
local function player_of(gc) return gc ~= 0 and u32(gc + GC_PLAYER) or 0 end
-- specialization 1: the arrow at 1.5 times the game's speed; 7: the same arrow at 0.04 times, which stays on the screen for the
-- length of a picture (what an arrow looks like in flight)
local function is_armed(p)
  if p == 0 or u8(p + P_CLASS) ~= ARMED_CLASS then return false end
  local spec = u8(p + P_SPEC)
  return spec == ARMED_SPEC or spec == 7
end

-- the World's creature map (a std::map at World+4: head node, left = first)
local function creatures(world)
  local out = {}
  local head = u32(world + 4)
  if head == 0 then return out end
  local node, n = u32(head), 0
  while node ~= head and n < 5000 do
    local c = u32(node + 0x18)
    if c ~= 0 then out[#out + 1] = c end
    local right = u32(node + 8)
    if u8(right + 0x0D) == 0 then
      node = right
      while true do
        local l = u32(node)
        if u8(l + 0x0D) ~= 0 then break end
        node = l
      end
    else
      local p = u32(node + 4)
      while u8(p + 0x0D) == 0 and node == u32(p + 8) do node = p; p = u32(node + 4) end
      node = p
    end
    n = n + 1
  end
  return out
end

-- ---- the targets -------------------------------------------------------------------------------------------------------
local targets = nil
local function line_up(gc, p)
  if not targets then
    local list = creatures(gc + GC_WORLD)
    targets = {}
    for _, c in ipairs(list) do
      if c ~= p and #targets < 3 then targets[#targets + 1] = c end
    end
    kyubu.info(("pierce_proto: %d target(s) lined up"):format(#targets))
  end
  for i, c in ipairs(targets) do
    local pos = ffi.cast("int64_t*", c + P_POS)
    local ppos = ffi.cast("int64_t*", p + P_POS)
    pos[0] = ppos[0]
    pos[1] = ppos[1] + i * 8 * BLOCK
    pos[2] = ppos[2]
    ffi.cast("uint8_t*", c + P_HOSTILE)[0] = 1
    ffi.cast("int32_t*", c + P_STUN)[0] = 600000
    ffi.cast("float*", c + P_HPMULT)[0] = 10000
    ffi.cast("float*", c + P_HP)[0] = 1000
  end
end

-- ---- the bar-built site: bridge and targets ----------------------------------------------------------------------------
local seen_mode = -1
local id_bar, e_bar = kyubu.hook_mid(0x48BFDC, function(ctx)
  local gc = u32(ctx.esp + 0x10)
  local p = player_of(gc)
  if p == 0 then return end
  local mode = u8(p + P_MODE)
  if mode ~= seen_mode then
    kyubu.info(("pierce_proto: mode %d -> %d (mode time %d, mp %.3f, armed %s)"):format(seen_mode, mode, i32(p + P_TIME), f32(p + P_MP),
      tostring(is_armed(p))))
    seen_mode = mode
  end
  if u8(p + P_CLASS) ~= ARMED_CLASS then return end
  line_up(gc, p)
  if not is_armed(p) then return end
  local keys = ffi.cast("uint8_t*", gc)
  if keys[4] ~= 0 and keys[5] == 0 then keys[5] = 1 end
end)
kyubu.info("pierce_proto: bar site " .. (id_bar and "hooked" or ("FAILED: " .. tostring(e_bar))))

-- ---- the release: mode 55 (a volley) becomes mode 23 (one arrow) -------------------------------------------------------
local id_rel, e_rel = kyubu.hook_mid(0x49C069, function(ctx)
  local p = ctx.eax
  if is_armed(p) then ffi.cast("uint8_t*", p + P_MODE)[0] = 23 end
end)
kyubu.info("pierce_proto: release site " .. (id_rel and "hooked" or ("FAILED: " .. tostring(e_rel))))

-- ---- the arrow, before it is pushed ------------------------------------------------------------------------------------
local function is_pierce(proj)
  return i32(proj + 0x60) == 1 and u8(proj + 0x5C) == 0
end
local id_arrow, e_arrow = kyubu.hook_mid(0x615E03, function(ctx)
  local frame = ctx.ebp
  local creature = u32(frame - 0x2B14)
  if not is_armed(creature) or u8(creature + P_MODE) ~= 23 then return end
  local proj = frame - 0x578
  ffi.cast("int32_t*", proj + 0x60)[0] = 1
  ffi.cast("uint8_t*", proj + 0x5C)[0] = 0
  local factor = u8(creature + P_SPEC) == 7 and 0.04 or 1.5
  for o = 0x3C, 0x44, 4 do ffi.cast("float*", proj + o)[0] = f32(proj + o) * factor end
  kyubu.info(("pierce_proto: arrow made: kind 1, skill 0, velocity (%.1f, %.1f, %.1f), damage %.2f"):format(f32(proj + 0x3C), f32(proj + 0x40),
    f32(proj + 0x44), ctx.xmm[1].f32[0]))
end)
kyubu.info("pierce_proto: arrow site " .. (id_arrow and "hooked" or ("FAILED: " .. tostring(e_arrow))))

-- ---- the flight: which arrow the loop is on, the stop flag, the hit set ----------------------------------------------------
local current = nil            -- address of the projectile the loop is on
local sets = {}                -- [address] = { age =, hit = { [creature id] = true } }
local steps = 0
local id_loop, e_loop = kyubu.hook_mid(0x61F31D, function(ctx)
  local proj = ctx.edi
  current = nil
  if is_pierce(proj) and i64(proj) == i64(player_of(controller()) + P_ID) then
    current = proj
    local age = i32(proj + 0x68)
    local s = sets[proj]
    if not s or age < s.age then s = { age = age, hit = {} }; sets[proj] = s end
    s.age = age
    steps = steps + 1
    kyubu.info(("pierce_proto: arrow step %d: age %d pos (%.2f, %.2f, %.2f) blocks"):format(steps, age,
      i64(proj + 0x18) / BLOCK, i64(proj + 0x20) / BLOCK, i64(proj + 0x28) / BLOCK))
  end
end)
kyubu.info("pierce_proto: loop site " .. (id_loop and "hooked" or ("FAILED: " .. tostring(e_loop))))

local id_stop, e_stop = kyubu.hook_mid(0x62076E, function(ctx)
  if current and ctx.edi == current then ffi.cast("uint8_t*", ctx.ebp - 0x2B64)[0] = 0 end
end)
kyubu.info("pierce_proto: stop site " .. (id_stop and "hooked" or ("FAILED: " .. tostring(e_stop))))

local id_surf, e_surf = kyubu.hook_mid(0x62094E, function(ctx)
  if current and ctx.edi == current then
    ffi.cast("int32_t*", ctx.edi + 0x60)[0] = 0
    kyubu.info("pierce_proto: arrow at a surface: kind back to 0")
  end
end)
kyubu.info("pierce_proto: surface site " .. (id_surf and "hooked" or ("FAILED: " .. tostring(e_surf))))

-- hitCreature, 14 stack arguments (ret 0x38): target, attacker, damage, crit, skill, mana, pos, ?, step output, effects, ?, 0,
-- the projectile (at the arrow's call site), 1. All passed on as 32-bit words.
local hits = 0
local ok, err = pcall(function()
  kyubu.hook("cube::World::hitCreature",
    "uint8_t(__thiscall*)(void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t)",
    function(nxt, this, target, attacker, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, proj, a14)
      if current and proj == current then
        local id = i64(target + P_ID)
        local s = sets[current]
        if s then
          if s.hit[id] then return 0 end
          s.hit[id] = true
          hits = hits + 1
          kyubu.info(("pierce_proto: arrow hit %d: creature %d"):format(hits, id))
        end
      end
      return nxt(this, target, attacker, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, proj, a14)
    end)
end)
kyubu.info("pierce_proto: hitCreature detour " .. (ok and "installed" or ("FAILED: " .. tostring(err))))

-- every hit of the World, as the game records it
local id_list, e_list = kyubu.hook_mid("cube::World::hitListSite", function(ctx)
  local hit = ctx.esp + 0xC4
  kyubu.info(("pierce_proto: hit recorded: attacker %d victim %d damage %.2f"):format(i64(hit), i64(hit + 8), f32(hit + 0x10)))
end)
kyubu.info("pierce_proto: hit list site " .. (id_list and "hooked" or ("FAILED: " .. tostring(e_list))))

-- ---- the draw: model, orientation and colour -------------------------------------------------------------------------------
-- The render loop over World.projectiles jumps on the kind (0 to 4) at 0x4B81A2 (ESI the projectile): kind 1 is a spell, a ball
-- drawn unturned with another model (slot 0x835); kind 0 an arrow, turned along its velocity, whose model is slot 0x348 (840), got
-- through ModelLoader::get at 0x4B83C8. So for the length of one projectile's draw a tagged arrow is made kind 0, and
-- the model the game took for it is changed for the mod's white arrow right after that call. 0x4B8B5C is where every path of
-- that loop ends.
local arrow_slot = nil
kyubu.on("lifecycle.register", function()
  local ok, why = kyubu.model.add("pierce_proto:arrow", "su_arrow_white.cub")
  kyubu.info("pierce_proto: model add: " .. tostring(ok) .. " " .. tostring(why))
end)
kyubu.on("lifecycle.game_ready", function()
  arrow_slot = kyubu.model.slot("pierce_proto:arrow")
  kyubu.info("pierce_proto: arrow model slot " .. tostring(arrow_slot))
end)

local drawn = nil              -- the tagged arrow whose draw is on
local draws = 0
local id_d1, e_d1 = kyubu.hook_mid(0x4B81A2, function(ctx)
  local proj = ctx.esi
  if proj == 0 then return end
  local p = player_of(controller())
  if p ~= 0 and is_pierce(proj) and i64(proj) == i64(p + P_ID) then
    ffi.cast("int32_t*", proj + 0x60)[0] = 0
    drawn = proj
    draws = draws + 1
    if draws == 1 then kyubu.info("pierce_proto: a tagged arrow is being drawn (first time)") end
  end
end)
kyubu.info("pierce_proto: draw start site " .. (id_d1 and "hooked" or ("FAILED: " .. tostring(e_d1))))
local id_d2, e_d2 = kyubu.hook_mid(0x4B8B5C, function(ctx)
  if drawn then
    ffi.cast("int32_t*", drawn + 0x60)[0] = 1
    drawn = nil
  end
end)
kyubu.info("pierce_proto: draw end site " .. (id_d2 and "hooked" or ("FAILED: " .. tostring(e_d2))))

-- cube::GameController::projectileArrowModelSite: the game has taken its arrow model (EAX) for the draw of the projectile in ESI.
-- ModelLoader::get is not detoured (it is called for every part of every creature). The World's ModelLoader is at
-- GameController+0x2F8, where the game's own draw takes it from: the first call logs whether it gives the same slot 840.
local get_model = nil
local model_log = 0
local id_d3, e_d3 = kyubu.hook_mid("cube::GameController::projectileArrowModelSite", function(ctx)
  if not drawn or ctx.esi ~= drawn or not arrow_slot then return end
  get_model = get_model or ffi.cast("void*(__thiscall*)(void*, int32_t)", kyubu.symbol("ModelLoader::get"))
  local loader = ffi.cast("void*", controller() + 0x2F8)
  local mine = tonumber(ffi.cast("uint32_t", get_model(loader, arrow_slot)))
  if model_log == 0 then
    model_log = 1
    kyubu.info(("pierce_proto: arrow model site: the game's model 0x%08X, slot 840 of GameController+0x2F8 0x%08X, the mod's 0x%08X"):format(
      ctx.eax, tonumber(ffi.cast("uint32_t", get_model(loader, 840))), mine))
  end
  if mine ~= 0 then ctx.eax = mine end
end)
kyubu.info("pierce_proto: arrow model site " .. (id_d3 and "hooked" or ("FAILED: " .. tostring(e_d3))))
