kyubu.on("frame.begin", function()
  local t = os.clock()
  while os.clock() - t < 0.010 do end
end)
