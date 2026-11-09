# Developer mods

These mods are for building and checking Kyubu, not for players. They are shipped in the SDK, never in the player
release or the catalog, and most of them do nothing outside dev mode.

Most are small, single-purpose experiments: each one exercises one game site or one API call so that a probe can
measure it. The probe records in `mapping/probes/` name the mods they install (`mods = [...]`), and `kmap probe replay`
runs them; that is how the entries in `mapping/` were checked. Do not remove one that a probe still names:
`kmap check --offline` fails when a record points at a missing mod.

| Kind | Mods | What they are |
|---|---|---|
| Tools | `kyubu_probe`, `auto_enter`, `teleport`, `give_item`, `anim_capture` | `kyubu_probe` is the replay's inspector, field finder and call tracer (the replay installs it itself). `auto_enter` skips the title menus so a check reaches a world in seconds. `teleport` and `give_item` are console commands. `anim_capture` is the game side of the Animator (`tools/animator`). |
| Prototypes (`*_proto`, `*_probe`) | the rest of the `*_proto` folders, `equip_gate_probe`, `velwrite_probe` | One experiment each; its `mod.toml` description says what it does and which probe uses it. |
| Fixtures | `mm_fixture_*`, `mp_fill`, `srefuse`, `su_dummy`, `su_rank`, `start_menu_row` | Fixed inputs for replays: mods that load, fail, need each other or hold a test catalog (ModManager), a full mp bar, a refused server ability, a joined player's rank. |

Each mod's own `mod.toml` has its description. To see which probes use a mod:

```bash
grep -l 'mods-dev/<name>"' mapping/probes/*.toml
```
