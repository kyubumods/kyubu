-- rig_read_proto, client side (task 7.2 of add-ability-animation, probe rig-read). draw_creature (0x4129C0) is a __thiscall with ten stack
-- arguments (ret 0x28): ECX is the draw record (Creature + 0x1498). Task 2.3 (static): the draw leaves a 4x4 matrix per part in the record,
-- 0x40 bytes each from rec + 0xEC (root) and rec + 0x12C (torso hub) up to rec + 0x4EC, and the model of each part in the slots rec + 0xBC..0xE8
-- (a float holding the model pointer). This mod (a) writes one vector of the record at the join site (0x41F135, EDI = the record) per phase,
-- with a rest phase between phases, and (b) after draw_creature returns for the local player, dumps the 17 matrices once per phase (the
-- sample time is SAMPLE_AT seconds into the phase, when the smoothing has settled), the model slots resolved to indexes of the model table
-- (ModelLoader = GameController + 0x2F8, its vector's begin at +4), and the appearance ids at Creature + 0x8C..0x9A. Nothing else is written.
-- Log lines: "rigread: first draw", "rigread: phase N label", "rigread: sample N label ...", "rigread: done".
local ffi = require("ffi")

local JOIN, DRAW = 0x41F135, 0x4129C0
local GC_PLAYER, GC_MODELS = 0x8006C8, 0x2F8
local WARMUP, PHASE_S, SAMPLE_AT = 25, 6, 4
local MAT0, MAT_STEP, MAT_COUNT = 0xEC, 0x40, 17

-- record offsets of the task 2.3 table (what the pose-join probe logged): root euler +0x60, root offset +0xAC, body position +0x6C, body euler +0x54,
-- head position +0x78, head euler +0x84, hand A position +0x0C / euler +0x24, hand B position +0x18 / euler +0x30
local ROOT_EUL, ROOT_OFF, BODY_POS, BODY_EUL, HEAD_POS, HEAD_EUL = 0x60, 0xAC, 0x6C, 0x54, 0x78, 0x84
local HA_EUL, HB_EUL = 0x24, 0x30
-- a phase is { label, { {offset, axis, value}, ... } }; axis 0 = x, 1 = y, 2 = z
local PHASES = {
  { "rest1", nil },
  { "head_euler_x_30", { { HEAD_EUL, 0, 30 } } },
  { "rest2", nil },
  { "head_euler_y_30", { { HEAD_EUL, 1, 30 } } },
  { "rest3", nil },
  { "head_euler_z_30", { { HEAD_EUL, 2, 30 } } },
  { "rest4", nil },
  { "head_pos_z_5", { { HEAD_POS, 2, 5 } } },
  { "rest5", nil },
  { "body_euler_x_30", { { BODY_EUL, 0, 30 } } },
  { "rest6", nil },
  { "body_pos_z_5", { { BODY_POS, 2, 5 } } },
  { "rest7", nil },
  { "root_euler_z_30", { { ROOT_EUL, 2, 30 } } },
  { "rest8", nil },
  { "root_offset_x_10", { { ROOT_OFF, 0, 10 } } },
  { "rest9", nil },
  { "handA_euler_x_30", { { HA_EUL, 0, 30 } } },
  { "rest10", nil },
  { "handB_euler_x_30", { { HB_EUL, 0, 30 } } },
  { "rest11", nil },
}

-- model slots (task 2.3): 0xBC head, 0xC0 hair, 0xC4 body, 0xC8 shoulder2, 0xCC hands, 0xD0 wings, 0xD4 tail, 0xD8 feet, 0xDC/0xE0 weapons, 0xE4 shoulders, 0xE8 neck
local SLOTS = { {"head", 0xBC}, {"hair", 0xC0}, {"body", 0xC4}, {"sh2", 0xC8}, {"hands", 0xCC}, {"wings", 0xD0}, {"tail", 0xD4},
                {"feet", 0xD8}, {"wpnB", 0xDC}, {"wpnA", 0xE0}, {"shoulders", 0xE4}, {"neck", 0xE8} }
-- appearance ids (int16) at Creature + N (atlas, creatures: head +0x8C, hair +0x8E, hands +0x90, feet +0x92, body +0x94, tail +0x96, shoulder2 +0x98, wings +0x9A)
local APPEAR = { {"head", 0x8C}, {"hair", 0x8E}, {"hands", 0x90}, {"feet", 0x92}, {"body", 0x94}, {"tail", 0x96}, {"sh2", 0x98}, {"wings", 0x9A} }

local function rd32(addr)
  local bytes = kyubu.read(addr, 4)
  return bytes and ffi.cast("uint32_t*", bytes)[0] or nil
end
local function gc()
  local p = kyubu.symbol("cube::game_controller")
  return p and rd32(p) or 0
end
local function player()
  local g = gc()
  return g ~= 0 and (rd32(g + GC_PLAYER) or 0) or 0
end

-- the model table: the vector's begin pointer at ModelLoader + 4, its end at +8; entries of 4 bytes (a Model*), per the atlas
local function table_index(ptr)
  if ptr == 0 then return -1 end
  local g = gc()
  local first, last = rd32(g + GC_MODELS + 4), rd32(g + GC_MODELS + 8)
  if not first or not last or first == 0 or last <= first then return -2 end
  local n = math.floor((last - first) / 4)
  local bytes = kyubu.read(first, n * 4)
  if not bytes then return -3 end
  local arr = ffi.cast("uint32_t*", bytes)
  for i = 0, n - 1 do if arr[i] == ptr then return i end end
  return -4
end

local t0, phase, phase_t, sampled, finished = nil, 0, 0, false, false
local want_sample, sample_phase = false, 0

local id_j, err_j = kyubu.hook_mid(JOIN, function(ctx)
  pcall(function()
    local rec = ctx.edi
    local creature = ffi.cast("uint32_t*", rec)[0]
    if creature == 0 or creature ~= player() then return end
    local now = os.time()
    if not t0 then t0 = now; kyubu.info("rigread: first draw of the player") end
    if now - t0 < WARMUP then return end
    if phase == 0 or (sampled and now - phase_t >= PHASE_S and phase <= #PHASES) then
      phase, phase_t, sampled = phase + 1, now, false
      if phase <= #PHASES then kyubu.info(("rigread: phase %d %s"):format(phase, PHASES[phase][1])) end
    end
    if phase > #PHASES then
      if not finished then finished = true; kyubu.info("rigread: done") end
      return
    end
    if not sampled and now - phase_t >= SAMPLE_AT then
      sampled = true
      want_sample, sample_phase = true, phase
    end
    for _, w in ipairs(PHASES[phase][2] or {}) do ffi.cast("float*", rec + w[1])[w[2]] = w[3] end
  end)
end)
kyubu.info(("rigread: mid hook at the join site 0x%X %s"):format(JOIN, id_j and "installed" or ("FAILED: " .. tostring(err_j))))

local function dump(rec, creature)
  local label = PHASES[sample_phase][1]
  kyubu.info(("rigread: sample %02d %s creature=%08X rec=%08X"):format(sample_phase, label, creature, rec))
  local out = {}
  for _, s in ipairs(SLOTS) do
    local ptr = ffi.cast("uint32_t*", rec + s[2])[0]
    if ptr ~= 0 then out[#out + 1] = ("%s=%08X(#%d)"):format(s[1], ptr, table_index(ptr)) end
  end
  kyubu.info("rigread: slots " .. table.concat(out, " "))
  out = {}
  for _, a in ipairs(APPEAR) do
    local v = ffi.cast("int16_t*", creature + a[2])[0]
    out[#out + 1] = ("%s=%d"):format(a[1], v)
  end
  kyubu.info("rigread: appearance " .. table.concat(out, " "))
  for k = 0, MAT_COUNT - 1 do
    local f = ffi.cast("float*", rec + MAT0 + MAT_STEP * k)
    local s = {}
    for i = 0, 15 do s[#s + 1] = ("%.3f"):format(f[i]) end
    kyubu.info(("rigread: m%02d rec+0x%X %s"):format(k, MAT0 + MAT_STEP * k, table.concat(s, " ")))
  end
end

-- after the original returns, the record holds the matrices of this draw
local CT = "void(__thiscall*)(void*, void*, void*, void*, void*, uint32_t, void*, void*, void*, uint32_t, void*)"
local id_d, err_d = kyubu.hook(DRAW, CT, function(next, this, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10)
  next(this, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10)
  if want_sample then
    pcall(function()
      local rec = tonumber(ffi.cast("uintptr_t", this))
      local creature = ffi.cast("uint32_t*", rec)[0]
      if creature ~= 0 and creature == player() then
        want_sample = false
        dump(rec, creature)
      end
    end)
  end
end)
kyubu.info(("rigread: function hook at draw_creature 0x%X %s"):format(DRAW, id_d and "installed" or ("FAILED: " .. tostring(err_d))))
