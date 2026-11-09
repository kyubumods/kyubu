# Kyubu

A mod loader for **Cube World Alpha v0.1.1** (July 2013). Kyubu replaces the game's `zlib1.dll` with a proxy that forwards zlib to the original and loads mods: asset packs, Lua scripts (LuaJIT) and native C/C++ DLLs. No other game file is modified; uninstalling restores the game byte-for-byte.

What mods can do with it:

- replace and add models, sounds, icons and text, with `.vox` models converted on the fly and dictionaries merged;
- react to game events (frames, rendering, input, assets, worlds, network) and cancel input;
- hook game functions, imports, virtual methods and single instructions through verified symbol maps, with chaining, priorities and conflict detection;
- register content under string ids that keep their numbers per save, and create their own registries;
- draw Dear ImGui windows, add console commands, and use a diagnostics panel and hot reload in dev mode;
- keep client and server logic apart (single-player runs both sides in one process), and talk between sides over channels, so the same mods can later run on a modded dedicated server.

A faulting mod is disabled while the game keeps running; a crash names the responsible mod and can disable it on the next start; safe mode starts without mods.

## Getting started

- Players: [docs/install.md](docs/install.md)
- Mod authors: [docs/README.md](docs/README.md), then `sdk/templates/`
- Building Kyubu: [docs/building.md](docs/building.md), tests in [tests/README.md](tests/README.md)

## Status

Version 1.0.2 (API 1.4), for the 2013-07-20 build of Cube World Alpha. Built with MinGW-w64 (i686) and checked on Windows 10 against the original `Cube.exe` and `Server.exe`; not yet built with Visual Studio.

- **Mods:** ModManager (its own repository, `mod_manager`), which the installer downloads from the mod catalog,
  adds a Mods row to the title menu. It lists, configures and switches mods, and installs verified mods from the signed catalog at <https://kyubumods.github.io/catalog/>. ModManager and the gameplay mods (SkillsUnleashed,
  EMTweaks, level_rework, title_plus) have their own repositories and reach players through that catalog.
- **Multiplayer:** `Server.exe` runs mods' server side, and gate mods (`gate = true`, [docs/mods.md](docs/mods.md)) make a server refuse clients whose mods differ. Mods' own data still travels only through the vanilla protocol; the modded-server protocol is designed ([docs/networking-future.md](docs/networking-future.md)) but not built.
- **How the game was mapped:** every game function and field Kyubu uses is in `mapping/`, each with the probe that checked it (`mapping/probes/`, summarised in [docs/game/](docs/game/README.md)).

## Licences

Kyubu is released under the MIT licence ([LICENSE](LICENSE)); it covers the loader, the SDK, the tools and the mods in this repository. Third-party components keep their licences, included in `third_party/*/`: MinHook (BSD-2), LuaJIT (MIT), Dear ImGui (MIT), toml++ (MIT), nlohmann/json (MIT), miniz (MIT), Monocypher (BSD-2 or CC0), doctest (MIT). Cube World is © Picroma; Kyubu ships none of its files.
