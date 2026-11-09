-- prop_proto, client side (task 8.2d of add-skills-unleashed, probe world-model-draw): when the player is a ranger of
-- specialization 4, asks the server side once for the props, with the player's position and the point the crosshair is on. The
-- spawn call is the server side's.
--
-- The first prop comes down on the point under the crosshair, so the game names it (a floating name and a health bar): the name
-- comes from a list that cube::GameController::update fills once a frame with the near creatures whose hp is above 0, and the
-- creature under the crosshair is picked from that list only. With specialization 5 the prop's hp reads 0 from
-- cube::GameController::lookListFillSite to lookListFilledSite, the two ends of the loop that fills the list: it is left out and
-- has no name, and its own hp is back before anything else reads it.
local ffi = require("ffi")

local GC_PLAYER, GC_LOOKED = 0x8006C8, 0x800A68        -- GC_LOOKED: the id (int64) of the creature under the crosshair
local P_ID, P_POS, P_CLASS, P_SPEC, P_RAY, C_HP = 0x08, 0x10, 0x140, 0x141, 0x160, 0x16C

local function u8(a) return ffi.cast("uint8_t*", a)[0] end
local function u32(a) return ffi.cast("uint32_t*", a)[0] end

local asked = false
local prop = nil               -- { base =, id = } of the prop under the crosshair, from the server side (the same process)
local hush = false             -- specialization 5: the list is filled without the prop
local kept = nil               -- the prop's own hp while the list is filled
local frames, fills, filled, looked = 0, 0, 0, nil

kyubu.channel.register("prop_proto:ready", function(_, m)
  if type(m) == "table" and m.base then
    prop = { base = m.base, id = m.id }
    kyubu.info(("prop_proto: the prop under the crosshair is creature %d"):format(m.id))
  end
end)

local id, err = kyubu.hook_mid(0x48BFDC, function(ctx)
  local gc = u32(ctx.esp + 0x10)
  local p = gc ~= 0 and u32(gc + GC_PLAYER) or 0
  if p == 0 or u8(p + P_CLASS) ~= 2 then return end
  local spec = u8(p + P_SPEC)
  if spec ~= 4 and spec ~= 5 then return end
  if (spec == 5) ~= hush then
    hush = spec == 5
    kyubu.info("prop_proto: the look-at list is filled " .. (hush and "without the prop" or "as the game fills it"))
  end
  frames = frames + 1
  local now = tonumber(ffi.cast("int64_t*", gc + GC_LOOKED)[0])
  if now ~= looked then
    looked = now
    kyubu.info(("prop_proto: frame %d: the creature under the crosshair is id %d%s; the prop's hp %.1f; sites reached %d and %d times")
      :format(frames, now, prop and now == prop.id and " (the prop)" or "", prop and ffi.cast("float*", prop.base + C_HP)[0] or -1,
        fills, filled))
  end
  if frames % 200 == 0 then
    kyubu.info(("prop_proto: %d frames, the two sites reached %d and %d times"):format(frames, fills, filled))
  end
  if asked then return end
  asked = true
  local pos = ffi.cast("int64_t*", p + P_POS)
  local ray = ffi.cast("float*", p + P_RAY)
  local ok, sent = kyubu.channel.send("prop_proto:spawn", kyubu.channel.SERVER, {
    x = tonumber(pos[0]), y = tonumber(pos[1]), z = tonumber(pos[2]), player_id = tonumber(ffi.cast("int64_t*", p + P_ID)[0]),
    ax = tonumber(pos[0]) + ray[0] * 65536, ay = tonumber(pos[1]) + ray[1] * 65536, az = tonumber(pos[2]) + ray[2] * 65536 })
  kyubu.info(("prop_proto: asked the server side: %s %s (the aim is %.1f, %.1f, %.1f blocks from the player)")
    :format(tostring(ok), tostring(sent), ray[0], ray[1], ray[2]))
end)
kyubu.info("prop_proto: bar site " .. (id and "hooked" or ("FAILED: " .. tostring(err))))

local id_a, e_a = kyubu.hook_mid("cube::GameController::lookListFillSite", function()
  fills = fills + 1
  kept = nil
  if not (hush and prop) then return end
  local hp = ffi.cast("float*", prop.base + C_HP)
  if hp[0] > 0 then
    kept = hp[0]
    hp[0] = 0
  end
end)
kyubu.info("prop_proto: look list fill site " .. (id_a and "hooked" or ("FAILED: " .. tostring(e_a))))
local id_b, e_b = kyubu.hook_mid("cube::GameController::lookListFilledSite", function()
  filled = filled + 1
  if kept and prop then ffi.cast("float*", prop.base + C_HP)[0] = kept end
  kept = nil
end)
kyubu.info("prop_proto: look list filled site " .. (id_b and "hooked" or ("FAILED: " .. tostring(e_b))))
