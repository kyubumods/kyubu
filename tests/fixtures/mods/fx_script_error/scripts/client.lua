-- A handler that errors 25 times per frame: the loader must disable it (more than 20 errors within 1 second).
local t = nil
kyubu.on("fx_script_error:boom", function() return t.missing end)
local frames = 0
kyubu.on("frame.begin", function()
  frames = frames + 1
  if frames == 10 then kyubu.info("still running after the failing handler was disabled") end
  for _ = 1, 25 do kyubu.emit("fx_script_error:boom") end
end)
