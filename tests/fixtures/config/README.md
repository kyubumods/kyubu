# Config fixtures

Used by `tests/unit/test_config.cpp` (add-mod-config tasks 1.1, 1.2 and 2.x).

- `cfg_demo/`: a mod declaring `radius` (float, 1.0 to 32.0) and `mode` (string, "fire" or "ice").
- `cfg_demo_v2/`: the same mod after an update that declares a new option, `speed`.
- `cfg_bad_default/`: `type = "int"` with `default = "big"`; the mod must not load.
- `cfg_disabled/`: valid options, disabled in the test's `kyubu/config.toml`; no file may be written for it.
- `cfg_lua/`: a restricted-trust Lua script that reads and sets its own option.
- `cfg_plain/`: a mod without `config.toml`.
- `player/`: players' files for `cfg_demo`: a changed value, a value out of range, a syntax error on line 3, and a
  key the mod does not declare.
