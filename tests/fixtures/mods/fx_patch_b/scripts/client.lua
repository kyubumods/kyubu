local base = kyubu.symbol("ModelLoader::resizeSite")
local id, err = kyubu.patch(base + 3, kyubu.read(base + 3, 4))
kyubu.info("patch b: " .. tostring(id) .. " " .. tostring(err))
