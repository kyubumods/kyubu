# Kyubu documentation

Kyubu is a mod loader for Cube World Alpha v0.1.1. It loads mods made of assets, Lua scripts and native DLLs into `Cube.exe` (and `Server.exe`) without changing any game file except swapping `zlib1.dll`.

## Players

- [install.md](install.md): installing, adding mods, safe mode, crash recovery, `config.toml`, uninstalling.
- [config.md](config.md): changing a mod's options in `kyubu\config\<id>.toml`.
- the `mod_manager` repository: the MODS page on the title screen: enabling, installing, modpacks, undo.

## Mod authors

| Page | Covers |
|---|---|
| [mods.md](mods.md) | Mod packages, `mod.toml`, dependencies, load order, lifecycle phases. |
| [assets.md](assets.md) | Replacing and adding models, sounds, icons and text; providers; hot reload. |
| [config.md](config.md) | Mod options: `config.toml`, the player's `kyubu\config\<id>.toml`, validation, reading and setting options. |
| [lua.md](lua.md) | Lua mods: sides, the `kyubu` API, trust levels, FFI hooks, hot reload, editor stubs. |
| [native-api.md](native-api.md) | Native C/C++ mods: the SDK, entry points, ABI rules, result codes. |
| [events.md](events.md) | Every game event, with side, thread and payload. |
| [registries.md](registries.md) | String ids and numbers: models, text, custom registries, per-save id maps. |
| [abilities.md](abilities.md) | The fourth ability slot: defining an ability, claims, the placeholder, callbacks, ranks, limits. |
| [animations.md](animations.md) | Ability animation clips: the `.anim.toml` format, binding a clip to an ability, borrowing a vanilla pose, tracks (the frame and the order of a pose), tools, limits. |
| [game-api.md](game-api.md) | The game API: primitives and their availability, result codes, sides, owners, creature handles, threads; adding a function. |
| [hooks.md](hooks.md) | Symbol maps and hooking game code; the `hook_sample` walk-through. |
| [game/README.md](game/README.md) | Game reference: every mapped type, function and probe, generated from `mapping/`. |
| [sides-and-networking.md](sides-and-networking.md) | Client and server sides, side rules, channels, session manifest, vanilla servers. |
| [dev-tools.md](dev-tools.md) | Console, dev mode, diagnostics panel, notices, `kanim`, the Animator and the rig file format, `kyubu-voxedit` (the voxel model editor). |
| [probes.md](probes.md) | The `kyubu_probe` developer mod: inspector, field finder, call tracer; probe records and replays for mapping (native on Windows, Wine deprecated). |
| the `level_rework` repository | The `level_rework` gameplay mod: the level, XP, creature and item tables, the compatibility rules for saves and multiplayer, its options. |
| the `mod_manager` repository | ModManager: the Mods page, the change queue, the catalog, modpacks and `tools/kcat`; the `label` / `group` / `restart` option fields. |
| the `emtweaks` repository | The `emtweaks` gameplay mod (EMTweaks, the mod other mods depend on): the shockwave (a flat ring that expands and fades in the 3D world), the game's own particles by name (presets, the trace), their interfaces for other mods, the console commands and options, how it is drawn. |
| the `skills_unleashed` repository | The `skills_unleashed` gameplay mod (SkillsUnleashed): one fourth ability for every specialization, delivered one at a time; its tuning table, the effects and combat helpers, the bundler, the icon style envelope and how to build and check an icon, testing and the limits. |

## Kyubu developers

- [building.md](building.md): building, testing and packaging Kyubu itself.
- [networking-future.md](networking-future.md): the planned design for modded multiplayer.

## Quick start: a Lua mod in two minutes

1. Copy `sdk/templates/lua-mod` to `<Cube World>\mods\my_lua_mod`.
2. Start `Cube.exe --kyubu-dev`. A "My Lua Mod" window appears on the start menu.
3. Edit `scripts/client.lua` while the game runs; dev mode reloads it when you save.
