# Asset mods

Cube World keeps all of its content in four SQLite files, `data1.db` to `data4.db`, with every asset obfuscated. Kyubu never modifies those files. Instead it hooks the game's asset reader (`Database::get`) and serves mod files **as if they were in the databases**, re-encoding them the way the game expects. Removing a mod removes its assets; nothing needs rebuilding.

## Layout

Put files in the mod's `assets/` folder (another folder can be set with `assets = "..."` in `mod.toml`), one sub-folder per database:

| Folder | Database | File types | Asset key |
|---|---|---|---|
| `assets/models/` | `data1.db` | `.vox` (MagicaVoxel) or `.cub` | file name with `.cub`: `iron-sword1.vox` → `iron-sword1.cub` |
| `assets/sounds/` | `data2.db` | `.wav` | file name |
| `assets/icons/` | `data3.db` | `.png` | file name; an ability's `icon_file` names one ([abilities.md](abilities.md)) |
| `assets/text/` | `data4.db` | `dict_en.xml`, `dict_de.xml` | merged, see below |
| `assets/animations/` | none (read by the loader, not served to the game) | `.anim.toml` | `<mod id>:<file stem>`; an ability's `anim` names one ([animations.md](animations.md)) |

- A file named like a vanilla asset **replaces** it (`assets/models/iron-sword1.vox` reskins the iron sword).
- A file with a new name **adds** a key. New models become visible once a registry entry uses them (see [registries.md](registries.md)); a new key alone is never loaded by the game.
- Files in other folders, sub-folders, or with other extensions are ignored with a warning.

The asset-only example `mods-examples/gold_sword` reskins the iron sword gold and changes two quest lines:

```
gold_sword/
  mod.toml                          id, name, version, kyubu, sides = ["client"]
  assets/models/iron-sword1.vox
  assets/text/dict_en.xml
```

## Formats and validation

Every file is checked when it is first served. A file that fails is skipped with a log line naming the file and the problem, and the next layer (another mod or vanilla) serves the key instead, so a broken asset never reaches the game.

| Format | Accepted | Checks |
|---|---|---|
| `.vox` | MagicaVoxel files with a `SIZE`, `XYZI` and `RGBA` (palette) chunk; only the first model is used | size 1-256 per axis, at least one voxel |
| `.cub` | Cube World's model format: three little-endian `uint32` sizes (x, y, z), then one RGB triple per voxel, x fastest, then y, then z; `0,0,0` is empty | the file length matches the sizes, 1-256 per axis, at least one voxel |
| `.wav` | RIFF/WAVE, 16-bit PCM or 32-bit float | header and `fmt ` chunk |
| `.png` | PNG | signature |
| `dict_*.xml` | the game's dictionary format | parsed entry by entry |

`.vox` files are converted to `.cub` by the loader; the conversion is byte-for-byte the same as `cwmod-tools` (`tools/cwmod/cubvox.py`), which was checked on all 2,857 vanilla models. MagicaVoxel's Z axis is up, as in Cube World. A pure black voxel becomes `1,1,1`, because `0,0,0` means empty.

## Which file wins

For each key, Kyubu asks the layers in this order and serves the first one that has the key and passes validation:

1. `kyubu/dev-assets/<folder>/` (dev mode only; for quick experiments without making a mod),
2. mods, **later in load order first**,
3. asset providers (below), newest first,
4. vanilla.

Two mods replacing the same model is not an error: the later mod wins, and the log says which mod shadows which. The console command `assets <key>` shows who serves a key (`assets iron-sword1.cub` → `data1.db/iron-sword1.cub <- gold_sword`), and `api->asset_served_by` / `kyubu.assets.served_by` does the same from code.

## Text: dictionaries are merged, not replaced

`dict_en.xml` and `dict_de.xml` hold every string of the game as entries with a `key` attribute. A mod's dictionary only needs the entries it changes or adds:

```xml
<root>
  <speech key="quest-intro-warrior-1">
    Bring me the golden blade!
  </speech>
</root>
```

Kyubu starts from the vanilla dictionary and applies each mod's entries in load order: an entry with an existing key replaces it, a new key is added. Two mods that change different strings both take effect. Text registered through the text registry (`kyubu.text.add`, `registry_add("kyubu:text", ...)`) is merged the same way, after the files, as `<speech key="<id>">` entries in the matching language.

## Asset providers: assets from code

A provider serves keys that match a glob (`*` and `?`) from a function. It is asked only when no file layer has the key.

```lua
-- Lua: procedural models for every key "proc-*.cub"
kyubu.assets.provider("data1.db", "proc-*.cub", function(db, key)
  local sx, sy, sz = 4, 4, 12
  local parts = { string.char(sx, 0, 0, 0, sy, 0, 0, 0, sz, 0, 0, 0) }
  for z = 0, sz - 1 do for y = 0, sy - 1 do for x = 0, sx - 1 do
    parts[#parts + 1] = (z < 8) and string.char(90, 60, 30) or string.char(40, 160, 40)
  end end end
  return table.concat(parts)        -- or nil to decline
end)
```

```c
/* native: allocate the result with api->buffer_alloc; the loader frees it */
static KyubuResult KYUBU_CALL provide(const char* db, const char* key, void** out, uint32_t* len, void* user) {
    *out = api->buffer_alloc(size); /* fill it */ *len = size;
    return KY_OK;                    /* or KY_E_NOT_FOUND to decline */
}
api->asset_provider(self, "data1.db", "proc-*.cub", provide, NULL, &id);
```

Provider output is validated like files. Register providers during PreInit so they are in place before the game loads its models.

## Reading assets from code

`api->asset_read(db, key, buf, cap, &len)` / `kyubu.assets.read(db, key)` return an asset as the game would get it, through every layer (decoded; dictionaries merged). Lua also returns the layer that served it.

## Hot reload (dev mode)

With dev mode on, Kyubu polls the asset files of every mod and `kyubu/dev-assets/` twice a second. When a **model** changes, the new file is validated and the affected model slots are reloaded in the running game (through `Model::load`), so an edited sword updates in your hand. Added files are picked up too. Sounds, icons and text are re-served on their next read; the game reads most of them only at startup, so those need a restart. An animation clip (`assets/animations/*.anim.toml`) is read again when it changes and is drawn from the next frame ([animations.md](animations.md)).

## Logging

- At `GameReady` each mod with assets gets one summary line: `assets: overrides 1 vanilla key(s), adds 0 new key(s)`.
- A key served by one mod while another mod also has it is logged once with both names.
- With `loader.asset_trace = true`, every read is logged with the layer that served it (very verbose).
- The event `asset.request` lets a mod observe every key the game reads (see [events.md](events.md)).

## Tools

`tools/cwmod` (Python 3 + Pillow) works with the vanilla files:

```
python tools/cwmod/cwmod.py extract "<game dir>" work     # every asset into work/vanilla/{models,vox,sounds,icons,text}
python tools/cwmod/cubvox.py cub2vox iron-sword1.cub iron-sword1.vox   # and vox2cub, preview
```

Extract, copy the `.vox` you want to change into your mod's `assets/models/`, edit it in MagicaVoxel, and start the game. `work/vanilla/catalog.csv` lists every model with its slot number and size. (`cwmod.py build/install` can also patch the databases directly for use without Kyubu; with Kyubu, don't.)
