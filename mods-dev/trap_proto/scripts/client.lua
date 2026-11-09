-- trap_proto, client side (task 9.2a of add-skills-unleashed, probe trap-body). The question: can the Scout's trap be a creature of
-- the game's own (a "carrier") that gives the flight, the landing and a bounce for free, while the mod draws the trap itself?
--   flight    the carrier gets a position and a velocity (blocks and blocks a second) and is logged every World step: does the game
--             move a creature that stands still, does it fall, land (physics_flags bit 0) and slow down on the ground?
--   bounce    when the on-ground bit comes up after a fall faster than `min_fall`, the vertical velocity is written back, upward, as
--             `bounce` times the fall speed (the Lua bounce of lib/combat.set_vertical_velocity)
--   hide      the carrier's draw colour (vec4 at [ESP+0x14] of the draw_creature sites) gets its alpha and its green and blue scaled
--             by `alpha`, `green`: is a creature seen through?  (lib/sites.on_tint scales only rgb.)
-- The carrier is the creature with the lowest id that is not the player: the newest spawn (entity_spawner:spawner GREEN_SLIME 1 still).
-- Console: trap_proto:run (timeline, about 40 s), trap_proto:opt <name> <value>. Offsets are real offsets of cube::Creature (+0x10).
local ffi = require("ffi")
local bit = require("bit")

local C_ID, C_POS, C_VEL, C_EXTRA, C_FLAGS, C_HOSTILE = 0x08, 0x10, 0x34, 0x4C, 0x5C, 0x60
local W_PLAYER = 0xB8
local N_LEFT, N_PARENT, N_RIGHT, N_ISNIL, N_VALUE = 0x00, 0x04, 0x08, 0x0D, 0x18
local FRAME_DT = 8
local BLOCK = 65536
local SITE_DRAW_A, SITE_DRAW_B, SITE_DRAW_C = 0x4B5D8C, 0x4BA8D8, 0x4BA97F

local opt = { bounce = 0.55, min_fall = 1.5, alpha = 1.0, green = 1.0, hide = 0, throw_vy = 5.0, throw_vz = 6.0, ahead = 2.0, up = 1.5, east = 2.0, mask = 0, shrink = 0 }
local C_SCALE = 0x80                -- AppearanceData.scale (CreatureData + 0x68 + 0x8, shifted by 0x10)
local orig_scale, mask_seen = nil, false
local carrier_id = nil
local timeline = nil
local elapsed = 0
local flight = nil               -- { t, was_ground, last_vz, bounces, steps, since }
local counts = { steps = 0, draw_a = 0, draw_b = 0, draw_c = 0 }

local function u8(a) return ffi.cast("uint8_t*", a)[0] end
local function u32(a) return ffi.cast("uint32_t*", a)[0] end
local function i32(a) return ffi.cast("int32_t*", a)[0] end
local function f32(a) return ffi.cast("float*", a)[0] end
local function i64(a) return tonumber(ffi.cast("int64_t*", a)[0]) end
local function put_f32(a, v) ffi.cast("float*", a)[0] = v end

-- the std::map at World+4 (key at +0x10, creature at +0x18)
local function each_creature(world, fn)
  local head = u32(world + 4)
  if head == 0 then return end
  local node, n = u32(head + N_LEFT), 0
  while node ~= head and n < 20000 do
    local c = u32(node + N_VALUE)
    if c ~= 0 then fn(c) end
    local right = u32(node + N_RIGHT)
    if u8(right + N_ISNIL) == 0 then
      node = right
      while true do
        local l = u32(node + N_LEFT)
        if u8(l + N_ISNIL) ~= 0 then break end
        node = l
      end
    else
      local p = u32(node + N_PARENT)
      while u8(p + N_ISNIL) == 0 and node == u32(p + N_RIGHT) do node = p; p = u32(node + N_PARENT) end
      node = p
    end
    n = n + 1
  end
end

local function find_carrier(world, player)
  local best, best_id = 0, nil
  each_creature(world, function(c)
    local id = i64(c + C_ID)
    if c ~= player and (best_id == nil or id < best_id) then best, best_id = c, id end
  end)
  return best, best_id
end

local function carrier_by_id(world)
  local found = 0
  each_creature(world, function(c) if i64(c + C_ID) == carrier_id then found = c end end)
  return found
end

local function place_and_throw(world, player)
  local c, id = find_carrier(world, player)
  if c == 0 then kyubu.warn("trap_proto: no creature but the player: spawn one first"); return end
  carrier_id = id
  -- the player faces north in the replay (body_yaw 0): `ahead` blocks north and `up` above the player's feet
  local pos = ffi.cast("int64_t*", c + C_POS)
  local ppos = ffi.cast("int64_t*", player + C_POS)
  kyubu.info(("trap_proto: before the throw the carrier is %.2f east, %.2f north, %.2f up of the player"):format((tonumber(pos[0]) - tonumber(ppos[0])) / BLOCK, (tonumber(pos[1]) - tonumber(ppos[1])) / BLOCK, (tonumber(pos[2]) - tonumber(ppos[2])) / BLOCK))
  pos[0], pos[1], pos[2] = ppos[0] + opt.east * BLOCK, ppos[1] + opt.ahead * BLOCK, ppos[2] + opt.up * BLOCK
  put_f32(c + C_VEL, 0); put_f32(c + C_VEL + 4, opt.throw_vy); put_f32(c + C_VEL + 8, opt.throw_vz)
  flight = { t = 0, was_ground = false, last_vz = opt.throw_vz, peak = 0, bounces = 0, steps = 0, since = 0 }
  kyubu.info(("trap_proto: thrown id %d hostile %d from (%.2f, %.2f, %.2f) with velocity (0, %.1f, %.1f); player at z %.2f")
    :format(id, u8(c + C_HOSTILE), tonumber(pos[0]) / BLOCK, tonumber(pos[1]) / BLOCK, tonumber(pos[2]) / BLOCK,
      opt.throw_vy, opt.throw_vz, tonumber(ppos[2]) / BLOCK))
end

local function watch(c, dt)
  local f = flight
  f.t, f.steps, f.since = f.t + dt, f.steps + 1, f.since + dt
  local flags = u32(c + C_FLAGS)
  local ground = bit.band(flags, 1) ~= 0
  local vz = f32(c + C_VEL + 8)
  if vz < f.peak then f.peak = vz end
  if ground and not f.was_ground and f.peak < -opt.min_fall and opt.bounce > 0 then
    local up = -f.peak * opt.bounce
    f.peak = 0
    put_f32(c + C_VEL + 8, up)
    f.bounces = f.bounces + 1
    kyubu.info(("trap_proto: t=%d landed: bounce %d with vz %.2f"):format(f.t, f.bounces, up))
    vz = up
  end
  f.was_ground = ground
  f.last_vz = vz
  if opt.shrink > 0 then
    if not orig_scale then
      orig_scale = { f32(c + C_SCALE), f32(c + C_SCALE + 4), f32(c + C_SCALE + 8) }
      kyubu.info(("trap_proto: carrier scale before the shrink: %.3f %.3f %.3f"):format(orig_scale[1], orig_scale[2], orig_scale[3]))
    end
    for i = 0, 2 do put_f32(c + C_SCALE + 4 * i, orig_scale[i + 1] * opt.shrink) end
  elseif orig_scale then
    for i = 0, 2 do put_f32(c + C_SCALE + 4 * i, orig_scale[i + 1]) end
    orig_scale = nil
  end
  if f.since >= 250 then
    f.since = 0
    local pos = ffi.cast("int64_t*", c + C_POS)
    kyubu.info(("trap_proto: t=%d pos (%.2f, %.2f, %.2f) vel (%.2f, %.2f, %.2f) extra (%.2f, %.2f) flags 0x%X ground %s")
      :format(f.t, tonumber(pos[0]) / BLOCK, tonumber(pos[1]) / BLOCK, tonumber(pos[2]) / BLOCK,
        f32(c + C_VEL), f32(c + C_VEL + 4), f32(c + C_VEL + 8), f32(c + C_EXTRA), f32(c + C_EXTRA + 4), flags, tostring(ground)))
  end
end

local pending = nil
local function on_step(ctx)
  local world, player = ctx.edi, ctx.eax
  if world == 0 or player == 0 or player ~= u32(world + W_PLAYER) then return end
  local dt = i32(ctx.ebp + FRAME_DT)
  counts.steps = counts.steps + 1
  elapsed = elapsed + dt
  if timeline then
    while timeline.i <= #timeline.events and elapsed - timeline.t0 >= timeline.events[timeline.i].at do
      local e = timeline.events[timeline.i]
      timeline.i = timeline.i + 1
      kyubu.info(("trap_proto: timeline +%d ms: %s"):format(e.at, e.note))
      if e.opt then opt[e.opt[1]] = e.opt[2] end
      if e.what then pending = e.what end
    end
    if timeline.i > #timeline.events then timeline = nil end
  end
  if pending then
    local what = pending
    pending = nil
    if what == "throw" then place_and_throw(world, player) end
    if what == "kill" and carrier_id then
      local c = carrier_by_id(world)
      if c ~= 0 then
        put_f32(c + 0x16C, 0.0)
        kyubu.info(("trap_proto: hp of the carrier set to 0 (it was lit and standing at rest)"))
      end
    end
    if what == "stop" then
      flight = nil
      kyubu.info(("trap_proto: counters steps %d draw_a %d draw_b %d draw_c %d"):format(counts.steps, counts.draw_a, counts.draw_b, counts.draw_c))
    end
  end
  if flight and carrier_id then
    local c = carrier_by_id(world)
    if c == 0 then kyubu.info("trap_proto: the carrier is gone"); flight = nil else watch(c, dt) end
  end
end

-- draw_creature's colour vec4 at [ESP+0x14]: red, green, blue, alpha
local function tint(creature, ctx, counter)
  if opt.mask ~= 0 and creature ~= 0 and carrier_id and i64(creature + C_ID) == carrier_id then
    local m = ffi.cast("uint32_t*", ctx.esp + 0x20)
    if not mask_seen then mask_seen = true; kyubu.info(("trap_proto: hide mask before: 0x%X"):format(m[0])) end
    m[0] = 0xFFFFFFFF
    counts[counter] = counts[counter] + 1
  end
  if opt.hide == 0 or creature == 0 or not carrier_id or i64(creature + C_ID) ~= carrier_id then return end
  local col = ffi.cast("float*", u32(ctx.esp + 0x14))
  col[1] = col[1] * opt.green
  col[2] = col[2] * opt.green
  col[3] = col[3] * opt.alpha
  counts[counter] = counts[counter] + 1
end

local function install(name, site, fn)
  local id, err = kyubu.hook_mid(site, function(ctx)
    local ok, e = pcall(fn, ctx)
    if not ok then kyubu.warn(("trap_proto: %s failed: %s"):format(name, tostring(e))) end
  end)
  local where = type(site) == "number" and ("0x%X"):format(site) or tostring(site)
  kyubu.info(("trap_proto: hook %s at %s %s"):format(name, where, id and "installed" or ("FAILED: " .. tostring(err))))
end

install("step", "cube::World::abilityDispatchSite", on_step)
install("draw_a", SITE_DRAW_A, function(ctx) tint(ctx.esi, ctx, "draw_a") end)
install("draw_b", SITE_DRAW_B, function(ctx) tint(ctx.ecx, ctx, "draw_b") end)
install("draw_c", SITE_DRAW_C, function(ctx) tint(ctx.ecx, ctx, "draw_c") end)

kyubu.command("trap_proto:run", "trap_proto:run: throw the newest creature, bounce it and hide it in turn (about 40 s)", function(_, print)
  timeline = { t0 = elapsed, i = 1, events = {
    { at = 0,     note = "throw with bounces, hidden by the mask", what = "throw", opt = { "mask", 1 } },
    { at = 6000,  note = "the carrier's hp to 0: is it removed quietly?", what = "kill" },
    { at = 9000,  note = "stop", what = "stop" },
  } }
  print("run queued")
end)

kyubu.command("trap_proto:opt", "trap_proto:opt <name> <value>", function(args, print)
  local name, value = args:match("^(%S+)%s+(%S+)")
  local v = tonumber(value)
  if not name or opt[name] == nil or not v then return print("usage: trap_proto:opt <name> <number>; names: bounce min_fall alpha green hide throw_vy throw_vz ahead up east mask shrink") end
  opt[name] = v
  print(name .. " = " .. tostring(v))
end)

kyubu.info("trap_proto: loaded")
