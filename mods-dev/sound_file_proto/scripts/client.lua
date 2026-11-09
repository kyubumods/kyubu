-- sound_file_proto, client side (task 8.2f of add-skills-unleashed, probe sound-file).
-- play_sound (0x484820) picks a .wav name for an id, builds a std::string of it (24 bytes: 16 of buffer, size, reserve), asks the
-- sound engine (GameController+0x80070C, a cube::XAudio2Engine) to load it (vtable slot 7, +0x1C, thiscall, one pointer argument,
-- returns a handle) and plays the handle (slot 6, +0x18: handle, volume, pan, pitch, 0). The name is looked up in data2.db, where
-- the mod's assets/sounds/ are served. This mod makes the same two calls with the name of its own file.
local ffi = require("ffi")

local GC_PLAYER, GC_ENGINE, GC_VOLUME = 0x8006C8, 0x80070C, 0x184
local P_CLASS, P_SPEC = 0x140, 0x141
local NAME = "su_bow_test.wav"

local function u8(a) return ffi.cast("uint8_t*", a)[0] end
local function u32(a) return ffi.cast("uint32_t*", a)[0] end

local str = ffi.new("uint8_t[24]")
local function play(gc, volume, pitch, name)
  name = name or NAME
  local engine = u32(gc + GC_ENGINE)
  if engine == 0 then return nil, "no engine" end
  local vt = u32(engine)
  local load_sound = ffi.cast("uint32_t(__thiscall*)(void*, void*)", u32(vt + 0x1C))
  local play_handle = ffi.cast("void(__thiscall*)(void*, uint32_t, float, float, float, uint32_t)", u32(vt + 0x18))
  ffi.fill(str, 24)
  ffi.copy(str, name)
  ffi.cast("uint32_t*", str + 16)[0] = #name
  ffi.cast("uint32_t*", str + 20)[0] = 15
  local handle = load_sound(ffi.cast("void*", engine), str)
  if handle == 0 then return nil, "loadSound gave 0" end
  local option = ffi.cast("int32_t*", gc + GC_VOLUME)[0]
  play_handle(ffi.cast("void*", engine), handle, option * volume * 0.01, 0.0, pitch, 0)
  return handle
end

local frames, played = 0, 0
local id, err = kyubu.hook_mid(0x48BFDC, function(ctx)
  local gc = u32(ctx.esp + 0x10)
  local p = gc ~= 0 and u32(gc + GC_PLAYER) or 0
  if p == 0 or u8(p + P_CLASS) ~= 2 or u8(p + P_SPEC) ~= 3 then return end
  frames = frames + 1
  if frames % 12 == 1 and played < 6 then
    -- the first three: a name that is nowhere, a vanilla sound and the mod's own; then the mod's at three pitches
    local names = { "su_nowhere.wav", "hit.wav", NAME, NAME, NAME, NAME }
    local pitch = ({ 1.0, 1.0, 1.0, 1.25, 1.5, 1.0 })[played + 1]
    local ok, handle, why = pcall(play, gc, 1.0, pitch, names[played + 1])
    played = played + 1
    kyubu.info(("sound_file_proto: play %d %s pitch %.2f: %s"):format(played, names[played], pitch,
      ok and (handle and ("handle 0x%X"):format(handle) or tostring(why)) or ("failed: " .. tostring(handle))))
  end
end)
kyubu.info("sound_file_proto: bar site " .. (id and "hooked" or ("FAILED: " .. tostring(err))))
