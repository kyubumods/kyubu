# Saves

Atlas page for the `saves` subsystem: the files under `Save/`, who opens them and what is written to them.
Reference pages: [docs/game/saves.md](../../docs/game/saves.md).

## Purpose

Saves keep the player's characters, the list of worlds and, per world, the changed zones, the world time and the
discovered part of the map. Every save file is a SQLite database with the same single table as the asset files,
`blobs(key TEXT PRIMARY KEY, value BLOB)`, read and written through `cube::Database` ([core](core.md)). The world
subsystem ([world](world.md)) decides when zones are written.

## Key classes

- [`cube::Database`](../../docs/game/types/cube_Database.md) (verified, RTTI): one per open file. The world save is
  the Database at World+0xAC, the map save the one at map object+0x8000F0, the world list the one at
  GameController+0x1001008.
- [`cube::WorldInfo`](../../docs/game/types/cube_WorldInfo.md) (verified, RTTI; Cube.exe): one entry of the world list,
  40 bytes, with the world name at +8. The controller keeps a vector of pointers to them.
- [`cube::World`](../../docs/game/types/cube_World.md) (verified, RTTI): owns the world save.
- [`cube::WorldMap`](../../docs/game/types/cube_WorldMap.md) (verified, RTTI; Cube.exe): the world map, embedded in
  the controller at GameController+0x800D3C (`probe:world-map-save`). It owns the map save ([travel](travel.md)).

The files, as found in the game folder and in the strings:

| File | Opened by | Keys seen |
|---|---|---|
| `Save/characters.db` | the `cube::GameController` constructor (string); the Database at GameController+0x1001000 | `num` (the number of characters), then `0`, `1`, … one zlib-compressed character blob per slot (see "Character save format") |
| `Save/worlds.db` | the `cube::GameController` constructor (string) | `num` (the number of worlds), then one entry per world |
| `Save/world_<name>.db` | `cube::World::openSave()` | `time`, and `zone` plus the zone's coordinates for each saved zone |
| `Save/map_<name>.db` | `cube::WorldMap::openSave()` | `discovered` |

The game folder has only `characters.db` and `worlds.db`, both with the table and no rows (read with SQLite in
immutable mode, nothing written).

## Entry points

- `cube::World::openSave()` (verified in 7.5; Cube.exe 0x5A3C00, Server.exe 0x4D7FA0): takes the seed and the world's
  name, sets the seed, opens `Save/world_<name>.db` and reads `time`. Called by the `cube::GameController` constructor,
  the World constructor and [zone_loader_step](../../docs/game/world.md) in Cube.exe, and by `main` in Server.exe;
  every caller passes a World as `this`.
- `cube::WorldMap::openSave()` (verified in 7.5, `probe:world-map-save`; Cube.exe only; alias `Map::openSave`):
  opens `Save/map_<name>.db` and reads `discovered`, once the world save is open. Called only by zone_loader_step,
  on the controller's `cube::WorldMap`.
- `cube::World::saveZone()` (candidate, both builds): writes one changed zone.
- [delete_world_saves](../../docs/game/saves.md) (candidate, new; Cube.exe 0x482200): the "Delete World" action.
  Deletes `Save\world_<name>.db` and `Save\map_<name>.db` with `DeleteFileA`, removes the entry from the world list and
  rewrites `num` and the list in `worlds.db`. Takes the controller in ECX (`fastcall`); no direct caller, so probably
  reached through a UI connection.
- `cube::Database::setBuffer()` and `cube::Database::set()` (candidate): every write goes through them.

The character save and the zone blob (Cube.exe, read statically on 2026-09-30; all candidates):

- [save_character](../../docs/game/saves.md) (0x487770): saves the player to its slot.
  - It first stores the player's position in the current world's entry of the per-world position map.
  - It then copies the player into a temporary `cube::Creature` with [creature_data_copy](../../docs/game/creatures.md).
  - Finally it serializes, compresses and writes the blob under the decimal slot number. This is the "temporary
    creature on a worker thread" the creatures page saw.
  - Callers (2026-10-01): a 60 s autosave in the client update loop (call at 0x469FD3, only in a world and while a
    controller flag is set), a menu choice in `cube::GameController::onMouseUp()` (0x47E5B2),
    [client_disconnect](../../docs/game/network.md), [create_character](../../docs/game/saves.md) and 0x467300
    (probably shutdown). Under Wine the save file changed every 60 s in a world.
  - **The autosave runs on a worker thread, and the copy is made under the World's lock** (2026-10-12, a frozen game
    read with a debugger). `save_character` locks the World (0x59ACD0, the critical section inside it) at its start and
    unlocks it (0x59E770) after the copy; serializing, compressing and writing happen on the copy, outside the lock. The
    main thread holds the same lock while it updates the World. So a Lua detour of `save_character` deadlocks: the worker
    takes the Lua state, calls the game's function and waits for the World's lock, while the main thread, inside the lock,
    reaches a Lua hook and waits for the Lua state. It happened within minutes of play, at an autosave.
  - `cube::GameController::saveCopiedSite` (0x487918, candidate) is the instruction after the unlock: EBX is the copy that
    will be written, [EBP+0xC] the player. A mid hook there changes what is saved without touching the player and without
    waiting for a lock of the game.
  - **The slot argument (2026-10-05, probes `character-slot-played-0` and `-1`):** the first stack argument is the slot,
    the one the player was loaded from: 0 after SELECT on the first card, 1 after the second, in all three autosaves of a run.
    The name in the player (`CreatureData.name`, Creature+0x1168) was `testi` and `Tester`. A mod reads the slot from
    `GameController+0x800A04` or from this argument.
- [character_serialize](../../docs/game/saves.md) (0x44DE20) and [character_deserialize](../../docs/game/saves.md)
  (0x44C3C0): the blob format below, written through the writer of
  [save_writer_init](../../docs/game/saves.md) (0x44AE50), which stores format version 1.
- [save_buffer_compress](../../docs/game/saves.md) (0x449A70): zlib-compresses the buffer in place. The loader
  decompresses with 0x449B90.
- [load_character](../../docs/game/saves.md) (0x480B80): reads a slot through `cube::Database::getBuffer()`.
  - **Three callers (7.1):** the controller's constructor (one call per slot of `characters.db`, each into a new
    list creature), the return-to-menu branch of `cube::GameController::onMouseUp()` (the same loop again) and
    [character_select_click](../../docs/game/saves.md).
  - **A failed load:** a slot without a row leaves the creature as it was. There is no message and no error, so a
    refusal has to be added by the caller or by a hook.
- [create_character](../../docs/game/saves.md) (0x482670): builds the new character and stores it with save_character.
- [character_select_click](../../docs/game/saves.md) (0x484340): the click on SELECT or CREATE in the character
  list. For a slot inside the list it calls load_character with the player (`GameController+0x8006C8`) as the
  creature, so the player is read from `characters.db` again at that moment; it does not copy the list entry.
  [character_list_selected](../../docs/game/saves.md) (0x488080) returns the list entry of the selected slot.
- [character_world_map_index](../../docs/game/saves.md) (0x44B9F0): `operator[]` of the per-world position map.
  The map sits at `(Creature+0x1D28)+0x1C` (head node, count). The key is a u32 seed and a std::string; a node
  holds the key at +0x10 and the 24-byte value at +0x30. A mod adds its marker with it.
- [delete_character](../../docs/game/saves.md) (0x481BC0): rewrites `num`, deletes the selected slot (controller
  +0x800A04), then renames every later key one down. The later blobs are moved, not re-serialized.
- [zone_serialize](../../docs/game/saves.md) (0x44E9D0): the zone blob written by `cube::World::saveZone()`.
- world_list_save (0x487AF0) and create_world (0x482A00), both guesses: write `worlds.db`. They were only skimmed.

## Data flow

1. At start-up the controller's constructor names `worlds.db` and `characters.db`. It probably reads the world list
   there (`num`, then the entries) into its vector of `cube::WorldInfo`; not traced yet.
2. Creating a world adds a `cube::WorldInfo` and rewrites the list (zone_loader_step does this after it sets the new
   world up); deleting one runs delete_world_saves.
3. Entering a world opens the world save (`cube::World::openSave()`, reads `time`) and the map save
   (`cube::WorldMap::openSave()`, reads `discovered`).
4. While playing, changed zones are written with `cube::World::saveZone()` when they are unloaded, and the rest when
   the World is destroyed. When `time` and `discovered` are written back is not traced yet.
5. The saves don't go through [asset_decode](../../docs/game/core.md). A character blob is a plain zlib stream: the
   real save in the kyubu-play folder decompresses with zlib and parses byte for byte.
6. **Characters:**
   - [create_character](../../docs/game/saves.md) and every later save go through
     [save_character](../../docs/game/saves.md) → [character_serialize](../../docs/game/saves.md) →
     [save_buffer_compress](../../docs/game/saves.md) → `cube::Database::setBuffer()`.
   - Loading reverses it through [load_character](../../docs/game/saves.md) and
     [character_deserialize](../../docs/game/saves.md).
   - Deleting renames the keys ([delete_character](../../docs/game/saves.md)).
   - **The list:** the game keeps every character in a vector of `cube::Creature*` at `GameController+0x80097C`,
     loaded when the controller is built (and again when the player returns to the menu). The selected index is
     `GameController+0x800A04`. Choosing a character loads the slot a third time, into the player. The player's
     own style object (with the map) is copied back into the list entry when the controller is destroyed, and the
     60 s autosave and the return to the menu save the player under the selected slot.
7. **Entering a world:** [zone_loader_step](../../docs/game/world.md) looks up the character's per-world position
   map by the exact (seed, world name) key. On a hit it puts the player at the stored position, otherwise at the
   default spawn.
8. **Zones:** [zone_serialize](../../docs/game/saves.md) doesn't store creature levels. Spawn records are regenerated
   from the world seed on every load, and only two u32 of state per spawn are kept ([world](world.md)).

## Client and server

- Single-player: Cube.exe writes all four kinds of file under its own `Save/`.
- Server.exe writes only the world save (`Save/world_<name>.db`, the same `cube::World::openSave()` and
  `cube::World::saveZone()` code). It has no `worlds.db`, `characters.db` or map save strings.
- In multiplayer the client still keeps its characters in `characters.db`. Whether it saves anything of a server's
  world is open.

## Hook points of interest

- `cube::Database::setBuffer()` (candidate): every save write, with its key, in one place.
- `cube::World::openSave()` (verified): the world save is being opened; a mod can open its own per-world data next
  to it.
- `cube::World::saveZone()` (candidate): a zone is written.
- delete_world_saves (candidate): a world is deleted; a mod can delete its own per-world data.
- [save_character](../../docs/game/saves.md) and [character_serialize](../../docs/game/saves.md) (`save_character` verified on 2026-10-05, `character_serialize` candidate):
  - This is the one path every character blob takes, so it is where a mod adds or refreshes its own per-character data.
  - The spare `cube::CreatureData` fields are **not** saved, so they can't carry data across a save.
  - What survives a vanilla load and save is the per-world position map: the loader inserts every entry, the save
    updates only the current world's, and zone_loader_step looks up only exact keys. A mod entry with a key no world
    can have survives: in a Wine run on 2026-10-01 (Kyubu loader, no mods) it stayed byte-for-byte through four
    autosaves while the world's own entry was rewritten.

## Open questions

- The layout of the world-list entries, `time` and `discovered`; the zone blob's 20-byte records and object bytes.
  The character blob is answered (see "Character save format").
- Where `time` and `discovered` are written back. A tracer probe on `setBuffer`. (Characters are saved every 60 s
  in a world; see save_character.)
- Whether Server.exe saves characters of connected players.
- What Creature+0x1198 (a float, 1.0 in the sample), +0x119C, +0x1304 (94 in the sample, probably coins) and +0x1308
  in the character blob are.

## Depth

reconnaissance, reviewed 2026-09-30

## Character save format

The value of each slot key in `characters.db`, after zlib decompression. It is written by
[character_serialize](../../docs/game/saves.md): values are little-endian, and strings are a u32 length plus bytes.
It was checked on 2026-09-30 against the save in the kyubu-play folder (character "testi", level 2): all 64,595 bytes
parse.

| # | Content | From |
|---|---|---|
| 1 | u32 format version, 1 | the writer |
| 2 | position, 3 × i64 | `cube::CreatureData.pos` |
| 3 | body roll, pitch and yaw, 3 × f32 | `cube::CreatureData.body_roll` to `body_yaw` |
| 4 | hp f32, current_xp i32, level i32, class_type u8, specialization u8 | `cube::CreatureData` |
| 5 | two u32 (1.0f and 0 in the sample) | Creature+0x1198, +0x119C |
| 6 | 13 × `cube::ItemData` (0x118 each, raw) | `cube::CreatureData.equipment` |
| 7 | name | `cube::CreatureData.name` |
| 8 | 20 bytes of the player style | Creature+0x1D28 |
| 9 | inventory: u32 tab count (4), then per tab a u32 slot count (50) and 0x11C bytes per slot | Creature+0x11DC |
| 10 | two u32 (94 and 0 in the sample) | Creature+0x1304, +0x1308 |
| 11 | known formulas: u32 count, then `cube::ItemData` each | style +0x14 list |
| 12 | per-world position map: u32 count, then per entry u32 world seed, world name, 3 × i64 last position | style +0x1C map |
| 13 | last world: u32 seed and its name | style +0x24, +0x28 |
| 14 | mana cubes u32, then u32 11 and 11 × u32 skill ranks | `cube::CreatureData.mana_cubes`, `cube::CreatureData.skills` |

No other `cube::CreatureData` field is saved. The spare fields that the network update carries are lost on save and
load ([network](network.md)). Item padding bytes are copied as they are and hold garbage.
