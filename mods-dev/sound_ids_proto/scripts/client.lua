-- sound_ids_proto, client side (task 2.7 of add-ability-animation, probe sound-ids). play_sound (0x484820) takes the sound id as its
-- first stack argument (a switch of 101 cases; id 32 is step2.wav and 33 is step-water.wav by static read) and no caller passes 32 or 33
-- as an immediate. This hook logs each call with the local player's current_mode and the caller, so the script's phases (idle, walk,
-- jump, swing) show what plays when. The hook only reads.
local ffi = require("ffi")

local GC_PLAYER, MODE = 0x8006C8, 0x68
local function u32(addr) return ffi.cast("uint32_t*", addr)[0] end
local function u8(addr) return ffi.cast("uint8_t*", addr)[0] end

local logged, MAX_LINES = 0, 600
local frame = 0
kyubu.on("frame.begin", function(ev) frame = ev.frame end)

local hook_id, herr = kyubu.hook_mid(0x484820, function(ctx)
  local ok, e = pcall(function()
    if logged >= MAX_LINES then return end
    local sp = ctx.esp
    local id = u32(sp + 4)
    local p = kyubu.symbol("cube::game_controller")
    local g = p and u32(p) or 0
    local player = g ~= 0 and u32(g + GC_PLAYER) or 0
    local mode = player ~= 0 and u8(player + MODE) or -1
    logged = logged + 1
    kyubu.info(("sound_ids: frame %d id %d mode %d caller %08X"):format(frame, id, mode, u32(sp)))
  end)
  if not ok and logged < MAX_LINES then logged = logged + 1; kyubu.info("sound_ids: hook failed: " .. tostring(e)) end
end)
kyubu.info(("sound_ids: mid hook at 0x484820 %s"):format(hook_id and "installed" or ("FAILED: " .. tostring(herr))))
