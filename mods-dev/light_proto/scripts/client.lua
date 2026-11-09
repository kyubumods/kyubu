-- light_proto (probe light-place). The Fire Mage's circle pushed a light record into the frame's light vector every frame and gave
-- no light in play, while the player's light showed (2026-10-18). This puts lights of its own at light_collect_site, in phases of
-- PHASE_MS each, so that a replay's screenshots show which of them light anything, and logs every record of the vector at the
-- start of each phase: the game's own lamp record (the player's lamp is lit by hand in phase `lamp`) beside the mod's.
local ffi = require("ffi")
local bit = require("bit")

local PHASE_MS = 8000
-- light_sort_site: before the sort and the hand-out to the terrain (run 4); runs 2 and 3 used light_collect_site, after it
local SITE = "light_sort_site"
local PHASES = {
  { name = "ready" },                                                  -- nothing of ours
  { name = "lamp", lamp = true },                                      -- the game's own lamp record, for its fields
  { name = "ahead", ahead = 4, up = 1, radius = 20, color = { 2.0, 0.6, 0.15 } },
  { name = "at-player", ahead = 0, up = 1.5, radius = 20, color = { 2.0, 0.6, 0.15 } },
  { name = "bright", ahead = 4, up = 1, radius = 20, color = { 8.0, 2.4, 0.6 } },
  { name = "high", ahead = 4, up = 6, radius = 30, color = { 2.0, 0.6, 0.15 } },
  { name = "lamp-copy", copy = true },                                 -- the lamp's own record, copied and moved 4 ahead
  { name = "off" },
}

-- the time of day, ms since midnight (int32): World + 0x80015C, read by the HUD's TIME (0x471B60); the World is GameController + 0x2DC.
-- Held at midnight, so that a light is not lost in daylight.
local GC_TIME, NIGHT_MS = 0x2DC + 0x80015C, 0
local GC_PLAYER, C_POS, C_YAW, C_FLAGS16, LAMP_BIT = 0x8006C8, 0x10, 0x30, 0x124, 0x200
local UNITS = 65536
local RECORD, VECTOR_AT = 0x30, 0xC8

local function u32(a) return ffi.cast("uint32_t*", a)[0] end

local function controller()
  local g = kyubu.symbol("cube::game_controller")
  return g and u32(g) or 0
end

local function player()
  local gc = controller()
  return gc ~= 0 and u32(gc + GC_PLAYER) or 0
end

-- push_back, read from the call at light_push_site (as lib/glow of skills_unleashed does it)
local function resolve_push()
  local site = kyubu.symbol("light_push_site")
  if not site then return nil, "no light_push_site" end
  local b = ffi.cast("const uint8_t*", site)
  local lea = { 0x8D, 0x8C, 0x24, 0xCC, 0x00, 0x00, 0x00, 0xE8 }
  for i, v in ipairs(lea) do if b[i - 1] ~= v then return nil, "light_push_site bytes" end end
  local at = site + 12 + ffi.cast("const int32_t*", site + 8)[0]
  local p = ffi.cast("const uint8_t*", at)
  if p[0] ~= 0x55 or p[1] ~= 0x8B or p[3] ~= 0x56 then return nil, ("target %08x"):format(at) end
  return ffi.cast("void(__fastcall*)(void*, void*, const void*)", at)
end

local push, why = nil, nil
local elapsed, phase_i, dumped = 0, 0, false
local record = ffi.new("uint8_t[?]", RECORD)
local lamp_copy = ffi.new("uint8_t[?]", RECORD)
local have_copy = false

local function dump(vec, label)
  local first, last = vec[0], vec[1]
  local n = first ~= nil and tonumber(last - first) / RECORD or 0
  kyubu.info(("light_proto: %s: %d record(s)"):format(label, n))
  for i = 0, n - 1 do
    local r = first + i * RECORD
    local q = ffi.cast("int64_t*", r)
    local f = ffi.cast("float*", r + 0x18)
    local w = ffi.cast("uint32_t*", r + 0x28)
    kyubu.info(("light_proto: %s #%d pos %.3f %.3f %.3f radius %.3f color %.3f %.3f %.3f +28 %.3f (%08x) +2C %08x"):format(label, i,
      tonumber(q[0]) / UNITS, tonumber(q[1]) / UNITS, tonumber(q[2]) / UNITS, f[0], f[1], f[2], f[3], f[4], w[0], w[1]))
  end
end

local function on_collect(ctx)
  local ph = PHASES[phase_i]
  if not ph or not push then return end
  local vec = ffi.cast("uint8_t**", ctx.esp + VECTOR_AT)
  local p = player()
  if p == 0 then return end
  if ph.lamp and not have_copy and vec[0] ~= vec[1] then
    ffi.copy(lamp_copy, vec[0], RECORD)                   -- the first is the nearest: the player's own lamp
    have_copy = true
  end
  local pushed = false
  local x, y, z = tonumber(ffi.cast("int64_t*", p + C_POS)[0]), tonumber(ffi.cast("int64_t*", p + C_POS + 8)[0]),
    tonumber(ffi.cast("int64_t*", p + C_POS + 16)[0])
  local yaw = math.rad(ffi.cast("float*", p + C_YAW)[0])
  local fx, fy = -math.sin(yaw), math.cos(yaw)
  if ph.color then
    ffi.fill(record, RECORD)
    local q = ffi.cast("int64_t*", record)
    q[0] = x + math.floor(fx * ph.ahead * UNITS)
    q[1] = y + math.floor(fy * ph.ahead * UNITS)
    q[2] = z + math.floor((ph.up - 0.9) * UNITS)          -- the position is the middle of the hitbox, 0.9 over the feet
    local f = ffi.cast("float*", record + 0x18)
    f[0], f[1], f[2], f[3] = ph.radius, ph.color[1], ph.color[2], ph.color[3]
    push(vec, nil, record); pushed = true
  elseif ph.copy and have_copy then
    ffi.copy(record, lamp_copy, RECORD)
    local q = ffi.cast("int64_t*", record)
    q[0] = x + math.floor(fx * 4 * UNITS)
    q[1] = y + math.floor(fy * 4 * UNITS)
    push(vec, nil, record); pushed = true
  end
  if not dumped then
    dumped = true
    dump(vec, ph.name)
  end
end

kyubu.on("lifecycle.game_ready", function()
  push, why = resolve_push()
  if not push then kyubu.warn("light_proto: no push_back: " .. tostring(why)) return end
  local id, err = kyubu.hook_mid(SITE, function(ctx)
    local ok, e = pcall(on_collect, ctx)
    if not ok then kyubu.warn("light_proto: " .. tostring(e)) end
  end)
  if not id then kyubu.warn("light_proto: hook: " .. tostring(err)) end
  kyubu.info("light_proto: hooked")
end)

kyubu.on("lifecycle.world_enter", function()
  elapsed, phase_i, dumped = 0, 0, false
  kyubu.info("light_proto: world")
end)

kyubu.on("frame.begin", function(ev)
  if phase_i == 0 and player() == 0 then return end
  local dt = ev.dt or 0
  if dt < 1 then dt = dt * 1000 end                        -- seconds or ms: both seen in the docs
  elapsed = elapsed + dt
  local want = math.floor(elapsed / PHASE_MS) + 1
  if want ~= phase_i and want <= #PHASES + 1 then
    phase_i = want
    dumped = false
    local ph = PHASES[phase_i]
    kyubu.info("light_proto: phase " .. (ph and ph.name or "done"))
  end
  local gc = controller()
  if gc ~= 0 then ffi.cast("int32_t*", gc + GC_TIME)[0] = NIGHT_MS end
  local ph = PHASES[phase_i]
  local p = player()
  if p ~= 0 and ph and ph.lamp then                        -- light the player's own lamp, every frame
    local a = ffi.cast("uint16_t*", p + C_FLAGS16)
    a[0] = bit.bor(a[0], LAMP_BIT)
  end
end)
