-- clone_proto, client side (task 10.2a of add-skills-unleashed, probe shadow-clone-draw). The question: can the Assassin's shadow
-- clones be the player drawn again, with no creature of their own? At creature_draw_site (ESI the creature, draw_creature's ten
-- arguments on the stack) the player's draw is followed by extra calls of draw_creature with:
--   matrix    a copy of argument 2 (the creature's matrix) moved by (dx, dy, dz) as render moves the dash's trail copies (0x424A60)
--   colour    a vec4 of our own: black
--   opacity   cube::CubeShader::setOpacity on argument 1 (the shader) before the draw, z-writes off below 1, both put back after
--   pose      the player's current_mode swapped for the number of clone_proto:dash (an ability on no bar whose clip holds a dash
--             pose) and the record's time for `pose_ms`, so the loader's pose provider draws that clip; the record's words +4 to
--             +0x4EC are saved before and written back after, so the player's own eased pose is not touched
--   yaw       the player's body yaw (+0x30) swapped for the extra draw: does a clone turn by it, or is the yaw in the matrix?
-- Console: clone_proto:run (a timeline of about 17 s, phases logged as "phase <X>").
local ffi = require("ffi")

ffi.cdef[[
typedef void (__fastcall *clone_draw_fn)(void* self, void* edx, void* a1, void* a2, void* a3, void* models, uint32_t frame_ms,
  void* colour, void* a7, void* a8, uint32_t hide_mask, void* a10);
typedef void (__fastcall *clone_opacity_fn)(void* shader, void* edx, float opacity);
typedef long (__stdcall *clone_render_state_fn)(void* device, uint32_t state, uint32_t value);
]]

local C_POS, C_YAW, C_MODE = 0x10, 0x30, 0x68
local W_PLAYER = 0xB8
local FRAME_DT = 8
local BLOCK = 65536
local RECORD = 0x1498                     -- the draw record inside the creature
local RECORD_KEEP_FROM, RECORD_KEEP_TO = 0x04, 0x4EC
local ROOT_SLOT = 0xEC                    -- the record's first part matrix (the root), as the last draw left it (probe rig-read)
local GC_DEVICE = 0x134                   -- GameController + 0x134: the IDirect3DDevice9 (render's SetRenderState calls)
local RS_ZWRITEENABLE, VT_SET_RENDER_STATE = 14, 57

local draw_fn = ffi.cast("clone_draw_fn", assert(kyubu.symbol("draw_creature"), "no draw_creature"))
local opa_at = kyubu.symbol("cube::CubeShader::setOpacity")
local opacity_fn = opa_at and ffi.cast("clone_opacity_fn", opa_at) or nil

local player = 0
local elapsed = 0
local timeline = nil
local started = false
local start_run                           -- set below
local AUTO_START_MS = 4000                -- the run starts by itself this long after the yaw write (a console step may be lost)
local yaw_seen_at = nil
local AUTO_START = false                  -- the shots lagged the self-started run by a minute (run 7): the console starts it
local dash_mode = nil
local clones = {}                         -- { dx, dy, dz, opacity, pose, yaw_add }
local logged_matrix = false
local counts = { draws = 0, extra = 0, errors = 0 }
local black = ffi.new("float[4]", 0, 0, 0, 1)
local matrix = ffi.new("float[16]")
local saved = ffi.new("uint8_t[?]", RECORD_KEEP_TO - RECORD_KEEP_FROM)

local function u8(a) return ffi.cast("uint8_t*", a)[0] end
local function u32(a) return ffi.cast("uint32_t*", a)[0] end
local function i32(a) return ffi.cast("int32_t*", a)[0] end
local function f32(a) return ffi.cast("float*", a)[0] end
local function vp(a) return ffi.cast("void*", a) end

local function move(m, x, y, z)
  m[12] = m[12] + m[0] * x + m[4] * y + m[8] * z
  m[13] = m[13] + m[1] * x + m[5] * y + m[9] * z
  m[14] = m[14] + m[2] * x + m[6] * y + m[10] * z
  m[15] = m[15] + m[3] * x + m[7] * y + m[11] * z
end

-- The matrix turned by `deg` (counter-clockwise from above) about the world point (cx, cy) before the view: M' = T(-c) Rz T(c) M.
local turned = ffi.new("float[16]")
local function turn(m, deg, cx, cy)
  if deg == 0 then return end
  local r = math.rad(deg)
  local c, s = math.cos(r), math.sin(r)
  -- rows 0 and 1 of Rz (row vectors): (c, s, 0) and (-s, c, 0); translation of T(-p) Rz T(p): p - p * Rz
  local tx = cx - (cx * c - cy * s)
  local ty = cy - (cx * s + cy * c)
  for j = 0, 3 do
    local a0, a1 = m[j], m[4 + j]
    turned[j] = c * a0 + s * a1
    turned[4 + j] = -s * a0 + c * a1
    turned[12 + j] = m[12 + j] + tx * a0 + ty * a1
  end
  for j = 0, 3 do m[j], m[4 + j], m[12 + j] = turned[j], turned[4 + j], turned[12 + j] end
end

local function set_zwrite(gc, on)
  local dev = u32(gc + GC_DEVICE)
  if dev == 0 then return end
  local fn = ffi.cast("clone_render_state_fn", u32(u32(dev) + 4 * VT_SET_RENDER_STATE))
  fn(vp(dev), RS_ZWRITEENABLE, on and 1 or 0)
end

local function extra_draws(ctx)
  local c, esp, gc = ctx.esi, ctx.esp, ctx.edi
  local a = {}
  for i = 0, 9 do a[i + 1] = u32(esp + 4 * i) end
  if not logged_matrix then
    logged_matrix = true
    local m = ffi.cast("float*", a[2])
    local p = ffi.cast("int64_t*", c + C_POS)
    kyubu.info(("clone_proto: player at (%.2f, %.2f, %.2f) yaw %.1f mode %d; args 0x%X 0x%X 0x%X 0x%X %d 0x%X 0x%X 0x%X 0x%X 0x%X")
      :format(tonumber(p[0]) / BLOCK, tonumber(p[1]) / BLOCK, tonumber(p[2]) / BLOCK, f32(c + C_YAW), u8(c + C_MODE),
        a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10]))
    for r = 0, 3 do
      kyubu.info(("clone_proto: matrix row %d: %.4f %.4f %.4f %.4f"):format(r, m[4 * r], m[4 * r + 1], m[4 * r + 2], m[4 * r + 3]))
    end
    local root = ffi.cast("float*", c + RECORD + ROOT_SLOT)
    for r = 0, 3 do
      kyubu.info(("clone_proto: root slot row %d: %.4f %.4f %.4f %.4f"):format(r, root[4 * r], root[4 * r + 1], root[4 * r + 2], root[4 * r + 3]))
    end
  end
  if #clones == 0 then return end
  local record = c + RECORD
  local mode, yaw = u8(c + C_MODE), f32(c + C_YAW)
  ffi.copy(saved, vp(record + RECORD_KEEP_FROM), RECORD_KEEP_TO - RECORD_KEEP_FROM)
  for _, k in ipairs(clones) do
    ffi.copy(matrix, vp(a[2]), 64)
    move(matrix, k.dx, k.dy, k.dz)
    if k.turn then
      local root = ffi.cast("float*", record + ROOT_SLOT)
      turn(matrix, k.turn, root[12], root[13])
    end
    if k.pose and dash_mode then
      ffi.cast("uint8_t*", c + C_MODE)[0] = dash_mode
      ffi.cast("uint32_t*", record + 4)[0] = k.pose
    end
    ffi.cast("float*", c + C_YAW)[0] = yaw + (k.yaw_add or 0)
    local see_through = k.opacity < 1 and opacity_fn
    if see_through then
      opacity_fn(vp(a[1]), nil, k.opacity)
      set_zwrite(gc, false)
    end
    local ok, err = pcall(draw_fn, vp(record), nil, vp(a[1]), matrix, vp(a[3]), vp(a[4]), a[5], black, vp(a[7]), vp(a[8]), a[9], vp(a[10]))
    if see_through then
      opacity_fn(vp(a[1]), nil, 1.0)
      set_zwrite(gc, true)
    end
    ffi.cast("uint8_t*", c + C_MODE)[0] = mode
    ffi.cast("float*", c + C_YAW)[0] = yaw
    ffi.copy(vp(record + RECORD_KEEP_FROM), saved, RECORD_KEEP_TO - RECORD_KEEP_FROM)
    if ok then counts.extra = counts.extra + 1 else
      counts.errors = counts.errors + 1
      if counts.errors <= 3 then kyubu.warn("clone_proto: extra draw failed: " .. tostring(err)) end
    end
  end
end
jit.off(extra_draws)

local function on_draw(ctx)
  if player == 0 or ctx.esi ~= player then return end
  counts.draws = counts.draws + 1
  extra_draws(ctx)
end
jit.off(on_draw)

local function phase(name, list)
  clones = list
  if player ~= 0 then
    local root = ffi.cast("float*", player + RECORD + ROOT_SLOT)
    local p = ffi.cast("int64_t*", player + C_POS)
    kyubu.info(("clone_proto: phase %s: root slot row 3 %.4f %.4f %.4f; row 0 %.4f %.4f %.4f; player (%.3f, %.3f, %.3f) yaw %.1f"):format(name,
      root[12], root[13], root[14], root[0], root[1], root[2], tonumber(p[0]) / BLOCK, tonumber(p[1]) / BLOCK, tonumber(p[2]) / BLOCK,
      f32(player + C_YAW)))
  end
  kyubu.info(("clone_proto: phase %s (%d clones; so far %d player draws, %d extra, %d errors)"):format(name, #list, counts.draws,
    counts.extra, counts.errors))
end

local function on_step(ctx)
  local world, c = ctx.edi, ctx.eax
  if world == 0 or c == 0 or c ~= u32(world + W_PLAYER) then return end
  player = c
  elapsed = elapsed + i32(ctx.ebp + FRAME_DT)
  -- the replay's script writes the body yaw 0 just before its marker `cl: A` (the player stood at -180): the run starts after that
  if AUTO_START and not started and math.abs(f32(c + C_YAW)) < 1 then
    yaw_seen_at = yaw_seen_at or elapsed
    if elapsed - yaw_seen_at >= AUTO_START_MS then
      started = true
      start_run()
    end
  end
  if not timeline then return end
  while timeline.i <= #timeline.events and elapsed - timeline.t0 >= timeline.events[timeline.i].at do
    local e = timeline.events[timeline.i]
    timeline.i = timeline.i + 1
    phase(e.name, e.clones)
  end
  if timeline.i > #timeline.events then timeline = nil end
end

local function install(name, site, fn)
  local id, err = kyubu.hook_mid(site, function(ctx)
    local ok, e = pcall(fn, ctx)
    if not ok then kyubu.warn(("clone_proto: %s failed: %s"):format(name, tostring(e))) end
  end)
  kyubu.info(("clone_proto: hook %s at %s %s"):format(name, site, id and "installed" or ("FAILED: " .. tostring(err))))
end

kyubu.on("lifecycle.register", function()
  local ok, err = kyubu.ability.define("clone_proto:dash", { duration_ms = 1000, cooldown_ms = 0, anim = "clone_proto:dash" })
  kyubu.info("clone_proto: define clone_proto:dash: " .. (ok and "ok" or tostring(err)))
end)

kyubu.on("lifecycle.game_ready", function()
  local n = kyubu.ability.number("clone_proto:dash")
  dash_mode = n
  kyubu.info(("clone_proto: clone_proto:dash is mode %s; setOpacity %s"):format(tostring(n), opa_at and ("0x%X"):format(opa_at) or "MISSING"))
  install("step", "cube::World::abilityDispatchSite", on_step)
  install("draw", "creature_draw_site", on_draw)
end)

start_run = function()
  local function two(o)
    local a = { dx = -2.5, dy = 6, dz = 0, opacity = 1, turn = 90 }
    local b = { dx = 2.5, dy = 6, dz = 0, opacity = 1, turn = -90 }
    for k, v in pairs(o) do a[k], b[k] = v, v end
    return { a, b }
  end
  timeline = { t0 = elapsed, i = 1, events = {
    { at = 0, name = "A", clones = {} },
    { at = 1000, name = "B", clones = two({}) },
    { at = 9000, name = "C", clones = two({ pose = 300 }) },
    { at = 17000, name = "D", clones = { { dx = 0, dy = 6, dz = 0, opacity = 1, pose = 300, turn = 180 },
                                         { dx = 3, dy = 3, dz = 0, opacity = 1, pose = 300, turn = 45 } } },
    { at = 25000, name = "E", clones = {} },
    { at = 28000, name = "done", clones = {} },
  } }
  started = true
  kyubu.info("clone_proto: run started")
end

kyubu.command("clone_proto:run", "clone_proto:run: draw the player's shadow clones in turn (about 28 s)", function(_, print)
  start_run()
  print("run queued")
end)

kyubu.info("clone_proto: loaded")
