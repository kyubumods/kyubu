-- 0x4ED0EB..0x4ED0EE: the operand of "push 0xA09" in ModelLoader::loadAll (same value written back)
local base = kyubu.symbol("ModelLoader::resizeSite")
local id, err = kyubu.patch(base + 1, kyubu.read(base + 1, 4))
kyubu.info("patch a: " .. tostring(id) .. " " .. tostring(err))
