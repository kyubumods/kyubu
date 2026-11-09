# Content registries

Cube World refers to content by number: model 899 is the iron sword. Mods that add content need numbers that do not collide with each other and that stay the same when a world is saved and loaded again, even after mods are added or removed. Registries solve this: mods name content with **string ids**, and Kyubu assigns the numbers.

## Ids and phases

- Every entry has an id `namespace:name`. A mod can only add ids in its own namespace (`<mod id>:...`); vanilla content is `cubeworld:...`. Names use lowercase letters, digits, `_`, `-`, `.` and `/`.
- Each entry can carry a **payload**: bytes whose meaning is defined by the registry (for models, the asset key to load).
- Registries follow the lifecycle ([mods.md](mods.md)):

| Phase | Allowed |
|---|---|
| PreInit | `registry_create` (custom registries) |
| Register (`lifecycle.register`) | `registry_add`, `registry_remap` (and `kyubu.model.*`, `kyubu.text.add`) |
| PostRegister and later | lookups: `registry_lookup`, `registry_name`, `registry_payload`, `registry_count`, `registry_entry`, `registry_hash` |

Calls in the wrong phase return `KY_E_PHASE`. Numbers do not exist before the registries are frozen at the end of Register.

- **Numbering** is deterministic: vanilla entries keep their numbers; mod entries follow, ordered by the owning mod's load order and then by id. The same mods produce the same numbers on every machine, which the registry hash (a SHA-256 over the sorted `id=number` lines) makes checkable.
- **Both sides.** Registries belong to the process and are shared by the client and server sides. Adding the same entry again with the same payload (typically once per side) is allowed; a different payload for an existing id is `KY_E_DUPLICATE`. Register everything on both sides so a dedicated server ends up with the same registries as a client.

## Built-in registries

| Registry | Entries | Payload |
|---|---|---|
| `kyubu:models` | 2,569 vanilla model slots (0-2568) named `cubeworld:<file>` (e.g. `cubeworld:iron-sword1` = 899, `cubeworld:airship` = 2563); mod models from 2569 on | asset key in `data1.db` |
| `kyubu:text` | mod text entries | JSON `{"en": "...", "de": "..."}` |
| `kyubu:sounds` | mod sounds | asset key in `data2.db` |
| `kyubu:icons` | mod icons | asset key in `data3.db` |
| `kyubu:abilities` | the vanilla abilities 0-110 named `cubeworld:<name>` (e.g. `cubeworld:smash` = 54; `cubeworld:mode_<n>` where the game's enum has no name), `kyubu:placeholder` = 111, mod abilities from 112 to 255 | the packed ability definition |

A few vanilla model files are loaded into more than one slot; the plain id names the lowest slot and `cubeworld:<file>#<slot>` names each one. `kyubu:missing` is Kyubu's placeholder model (a magenta-and-black cube) used for content whose mod is gone.

`kyubu:abilities` is non-persistent: it has no per-save id map, its numbers are assigned again at every start (a removed mod's numbers are free), and at most 144 mod abilities fit because the game's current-mode byte stops at 255; entries over the cap are left out at the freeze with an error. It takes entries only from the ability definition call: `registry_add` and `registry_remap` on it fail with `KY_E_INVALID_ARG` and a log line saying so.

`ability_define(self, id, def)` (Lua: `kyubu.ability.define(id, {...})`) is that call: full trust only (a `restricted` mod gets `KY_E_TRUST` and no `kyubu.ability` in Lua), either side, Register phase only, an id in the caller's namespace. The checks of a field (`duration_ms` 1 to 60,000, `cooldown_ms` 0 to 3,600,000 for each rank 0 to `max_rank`, `mp_cost` 0 to 1, `max_rank` 1 to 20, `slot` 4 or none, `class` 1 to 4, `specialization` any, 0 or 1, `icon` a vanilla ability) return `KY_E_INVALID_ARG` and the log names the field; a text id that is not in `kyubu:text` yet only warns. The same definition again (once per side) is `KY_OK`, other data under the id is `KY_E_DUPLICATE`. Lua evaluates a `cooldown_ms` function once, for the ranks 0 to `max_rank`, into the table the payload holds. The full guide is [abilities.md](abilities.md).

Four calls read the registry, on either side, without a mod handle, so natively they check no trust level (Lua: `kyubu.ability.number`, `name`, `list` and `slot_owner`, which a `restricted` mod does not have). `ability_number(id, &n)` and `ability_name(n, buf, cap)` convert between an id and its number; a mod's ability has a number only after the freeze (`KY_E_PHASE` before it), vanilla ones and `kyubu:placeholder` at once. `ability_list(index, id_buf, cap, &n, &count)` walks every ability in number order (`KY_E_PHASE` before the freeze, `KY_E_NOT_FOUND` at the end); Lua's `list()` returns `{ {id=, number=}, ... }`. `ability_slot_owner(class, specialization, id_buf, cap, &n)` gives the holder of slot 4 for a class (1 to 4) and a specialization (0 or 1; Lua also takes names such as `"warrior"` and `"guardian"`): the specialization's claim, else the class's `any` claim, else `kyubu:placeholder`, and `KY_E_NOT_FOUND` when nobody holds it. The console commands `ability list` and `ability slots` print the same data and write it to the log.

### Models

```lua
kyubu.on("lifecycle.register", function()
  kyubu.model.add("mymod:crate", "mymod-crate.cub")              -- new slot; the file is assets/models/mymod-crate.vox
  kyubu.model.remap("cubeworld:iron-sword1", "mymod-sword.cub")  -- the vanilla slot loads another key
end)
kyubu.on("lifecycle.game_ready", function()
  kyubu.info("crate is slot " .. kyubu.model.slot("mymod:crate"))
end)
```

```cpp
mod.add_model("mymod:crate", "mymod-crate.cub");        // kyubu.hpp
mod.remap_model("cubeworld:iron-sword1", "mymod-sword.cub");
uint32_t slot = mod.model_slot("mymod:crate");
```

What Kyubu does with them:

- The game sizes its model table for exactly 2,569 models (`ModelLoader::loadAll`). When it has loaded them, Kyubu grows the table to `2569 + <mod models>` (`ModelVector::resize`) and loads each mod model into its slot. The log says, for example, `Model slots: 2571 total: vanilla's 2569, 1 from mods, 1 Kyubu placeholder(s)` (the placeholder is `kyubu:missing`).
- A remap makes a vanilla slot load a different key (`slot 899 (cubeworld:iron-sword1) loads mymod-sword.cub (remap)`). When several mods remap the same slot, the last in load order wins and the log says so. For a plain reskin, replacing the asset file is simpler (see [assets.md](assets.md)); a remap is for pointing a slot at a key your mod generates or shares.
- A mod model whose asset is missing or invalid shows `kyubu:missing`; a remap whose key fails to load falls back to the vanilla model. Both are logged as errors; neither leaves a slot empty.

In this release the new slots are loaded and addressable, but the game's item, creature and equipment code still only refers to vanilla slots: making the game **show** a new model in place of an item needs hooks on that code (the model resolvers are candidate symbols, see [hooks.md](hooks.md)). Remaps and asset overrides are visible in game today.

### Text

```lua
kyubu.on("lifecycle.register", function()
  kyubu.text.add("mymod:greeting", "Hello!", "Hallo!")   -- English, German (defaults to English)
end)
-- later: kyubu.text.get("mymod:greeting") in the game's current language, or kyubu.text.get(id, "de")
```

Text entries are merged into the game's dictionaries as `<speech key="mymod:greeting">` (see [assets.md](assets.md)), so game code that looks up keys can find them.

## Custom registries

Any mod can create a registry in its namespace during PreInit; other mods add entries to it during Register, in their own namespaces. The payload format is up to the registry's owner; a JSON string or a `kyubu.pack`ed table works well. Persistent registries (the default) get per-save id maps like the built-in ones.

The two example mods `spells_lib` and `frost_spells` show the pattern. `spells_lib` creates the registry and lists it once it is frozen:

<!-- sample: mods-examples/spells_lib/scripts/common.lua -->
```lua
-- spells_lib: creates a registry other mods fill, then reads it back once it is frozen.

-- PreInit (top level): create the registry. Its name must be in this mod's namespace.
-- Registries are shared by both sides of a process; creating the same registry again from the
-- other side is allowed, which keeps the code the same for single-player and a dedicated server.
assert(kyubu.registry.create("spells_lib:spells", 1, true))

-- PostRegister: every mod has added its spells; numbers are assigned and stable.
kyubu.on("lifecycle.post_register", function()
  local n = kyubu.registry.count("spells_lib:spells") or 0
  for i = 0, n - 1 do
    local id, number = kyubu.registry.entry("spells_lib:spells", i)
    local payload = id and kyubu.registry.payload("spells_lib:spells", id)
    if payload then
      local spell = kyubu.unpack(payload)
      kyubu.info(("spell #%d %s: %s, %d mana"):format(number, id, spell.name, spell.mana))
    end
  end
end)
```

`frost_spells` depends on `spells_lib` (so it loads after it) and adds two spells:

<!-- sample: mods-examples/frost_spells/scripts/common.lua -->
```lua
-- frost_spells: adds entries to another mod's registry. Entry ids use this mod's namespace;
-- the payload is any string (here a packed Lua table).

kyubu.on("lifecycle.register", function()
  kyubu.registry.add("spells_lib:spells", "frost_spells:frost_nova", kyubu.pack({ name = "Frost Nova", mana = 30 }))
  kyubu.registry.add("spells_lib:spells", "frost_spells:ice_lance", kyubu.pack({ name = "Ice Lance", mana = 12 }))
end)

kyubu.on("lifecycle.game_ready", function()
  local number = kyubu.registry.lookup("spells_lib:spells", "frost_spells:ice_lance")
  kyubu.info("frost_spells:ice_lance is spell number " .. tostring(number))
end)
```

In game the log shows:

```
INFO  [spells_lib] [client] spell #0 frost_spells:frost_nova: Frost Nova, 30 mana
INFO  [spells_lib] [client] spell #1 frost_spells:ice_lance: Ice Lance, 12 mana
INFO  [frost_spells] [client] frost_spells:ice_lance is spell number 1
```

(and the same lines for the server side). The native equivalents are `api->registry_create`, `registry_add`, `registry_count`, `registry_entry`, `registry_payload` and `registry_lookup`.

## Per-save id maps

Numbers must not change under a saved world. When a single-player world is entered, Kyubu reads `Save\world_<name>.kyubu-idmap.json`:

```json
{
  "format": 1,
  "registries": {
    "kyubu:models": { "hello_native:crate": 2569, "kyubu:missing": 2570 },
    "kyubu:text": { "hello_lua:greeting": 0 }
  }
}
```

- Entries present now and in the file **keep the saved number**, even if the load order changed.
- Entries in the file whose mod is gone become **placeholders**: the number stays reserved (nothing else gets it), models show as `kyubu:missing`, and a notice names the missing mods ("This world uses content from missing mods: ..."). Reinstalling the mod restores the content.
- New entries get free numbers.
- If any number moved, the `registry.remapped` event fires before `lifecycle.world_enter`, so mods can refresh cached numbers.
- The file is written back on every world enter. It sits next to the world's save files and does nothing without Kyubu.

## Hashes and the session manifest

`registry_hash(name)` returns the SHA-256 of a frozen registry's `id=number` lines. The session manifest ([sides-and-networking.md](sides-and-networking.md)) lists the hash of each registry; a future Kyubu server will compare them during its handshake ([networking-future.md](networking-future.md)).
