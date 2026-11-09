-- icon_proto, client side (task 4.5 of add-ability-api, probe ability-icon-slot). One ability with an icon on the warrior's
-- slot 4 and one without on the ranger's, to see the slot show smash's icon and stay empty.
kyubu.on("lifecycle.register", function()
  if not kyubu.ability then kyubu.info("icon_proto: this loader has no kyubu.ability"); return end
  local ok, err = kyubu.ability.define("icon_proto:smasher", {
    icon = "cubeworld:smash",
    duration_ms = 800,
    claim = { slot = 4, class = "warrior", specialization = "any" },
  })
  kyubu.info(("icon_proto: define smasher: %s"):format(ok and "ok" or tostring(err)))
  ok, err = kyubu.ability.define("icon_proto:blank", {
    duration_ms = 800,
    claim = { slot = 4, class = "ranger", specialization = "any" },
  })
  kyubu.info(("icon_proto: define blank: %s"):format(ok and "ok" or tostring(err)))
end)
