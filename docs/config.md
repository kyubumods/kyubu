# Mod options

A mod declares its settings once, in `config.toml` in its package. Kyubu gives every player one file per mod to
change them in, `kyubu\config\<id>.toml`, checks every value, and lets the mod read and set its options through the
API. Mods no longer need their own file formats, and a player's values survive mod updates, even for zipped mods.

## Declaring options: `config.toml`

Put `config.toml` at the root of the mod's package (folder or zip), next to `mod.toml`. Each option is a table
`[options.<key>]`:

```toml
[options.radius]
type = "float"            # bool, int, float or string
default = 8.0             # required, of that type (an integer is a valid float)
min = 1.0                 # int and float only; optional
max = 32.0
description = "Blast radius in blocks."

[options.mode]
type = "string"
default = "fire"
choices = ["fire", "ice"] # string only; optional
description = "What the blast is made of."
```

| Field | Required | Meaning |
|---|---|---|
| `type` | yes | `"bool"`, `"int"` (64-bit), `"float"` or `"string"`. |
| `default` | yes | The value used until the player sets one, and whenever the player's value is not allowed. It must itself be allowed. |
| `description` | no | Written above the option in the player's file. Several lines are fine. |
| `min`, `max` | no | Limits for `int` and `float` options, inclusive. |
| `choices` | no | The only values a `string` option may take. |
| `label` | no | The name that a settings form shows for the option, at most 40 characters. Without it the form shows the key. |
| `group` | no | The heading under which a settings form lists the option. Options with the same `group` stay together. |
| `restart` | no | `true`: the mod reads the option only at start. The value is still saved at once; a settings form says "Takes effect at the next start". |

`label`, `group` and `restart` only change what a settings form shows. They do not change the player's file or the
validation. A `label` longer than 40 characters, or a value of the wrong type, is an error like any other field.

Keys match `^[a-z][a-z0-9_]{0,31}$`. `mod.toml` does not change: Kyubu finds the declaration by its file name.

A declaration that is wrong stops only that mod, like a bad `mod.toml`, and the log names the file and the key:

```
mods/blaster/config.toml: key 'options.radius.default': expected an int (type = "int"), got a string
```

Unknown fields (`defualt`) and unknown top-level keys are errors too, so typos do not go unnoticed.

## The player's file: `kyubu\config\<id>.toml`

The first time an enabled mod with options loads, Kyubu writes its file with every default, each under its
description, type and limits:

```toml
# Options of Blaster (blaster). Kyubu wrote this file from the mod's config.toml.
# Change the values here: they are kept when the mod is updated. See docs/config.md.

# Blast radius in blocks.
# float, 1.0 to 32.0, default 8.0
radius = 8.0

# What the blast is made of.
# string, one of "fire", "ice", default "fire"
mode = "fire"
```

- The file is read when the mod loads, before any of its entry points or scripts run.
- When an update declares a new option, it is appended with its default. Nothing else in the file changes.
- A value the player wrote is never overwritten by an update. A key the mod no longer declares stays in the file, and
  the log says once that it is unused.
- Deleting a line brings the default back: the next start appends it again.
- Disabled mods, and mods without `config.toml`, get no file.
- The file lives outside the package, so replacing the mod's folder or zip keeps it.

## Validation

A value of the wrong type, outside `min`/`max`, or not one of `choices` is reported with the file, the key, the value
and what is allowed, and the default is used instead. The mod still loads, and the file is not rewritten:

```
kyubu/config/blaster.toml: radius = 100.0 is not allowed (a number from 1.0 to 32.0); using the default 8.0
```

A file that is not valid TOML is reported with its line and column, and every option of that mod uses its default
until the file is fixed. Kyubu does not edit such a file (setting an option fails until it is fixed or deleted).

## Reading and setting options

A mod reads and sets only its own options, at every trust level. Setting validates the value like the player's file,
saves it, and changes only that key's line: comments and the other values stay as they are.

Lua:

```lua
local radius = kyubu.config.get("radius")           -- 8.0; nil, "why" for an undeclared key
local ok, why = kyubu.config.set("radius", 16)      -- true; nil, "why" when the value is not allowed
```

Native (API 1.1; check `api->size` first, or use `kyubu.hpp`, which does):

```c
double r;
if (api->config_get_float(self, "radius", &r) == KY_OK) { /* ... */ }
api->config_set(self, "radius", "16");              /* the value as text: "16", "true", "fire" */
```

```cpp
double r = mod.config_float("radius", 8.0);         // the fallback when the option is missing or has another type
mod.config_set("radius", 16.0);
```

| Native member | Returns |
|---|---|
| `config_get_bool`, `config_get_int`, `config_get_float`, `config_get_string` | `KY_OK`; `KY_E_NOT_FOUND` for an undeclared key; `KY_E_INVALID_ARG` when the option has another type; `KY_E_BUFFER` when a string does not fit. |
| `config_set` | `KY_OK`; `KY_E_NOT_FOUND`; `KY_E_INVALID_ARG` when the value is not allowed (nothing changes); `KY_E_UNSAFE` when the player's file is not valid TOML. |

Both sides of a mod share its options: there is one file per mod, not per side.

## `config.changed`

When an option's value changes after the mod's init (set through the API, the console, or a reload of the file), Kyubu
emits `config.changed` on the main thread, on the next frame, to every side the mod runs on. The payload is
`KyubuConfigChangedData { mod, key }` (`ev.mod`, `ev.key` in Lua). Other mods may subscribe too, so check the mod id.
Setting an option to the value it already has emits nothing.

A mod manager changes the options of any mod through `mods_option_set` ([mods.md](mods.md#managing-mods)). The loader
treats this as the mod's own `config_set`, so the mod gets `config.changed` as above, with no restart. Handle
`config.changed`, or mark an option `restart = true` when the mod reads it only at start.

```lua
kyubu.on("config.changed", function(ev)
  if ev.mod == kyubu.id and ev.key == "radius" then radius = kyubu.config.get("radius") end
end)
```

## The `config` console command

Available without dev mode:

| Command | Does |
|---|---|
| `config` | Lists the mods with options and their files. |
| `config <mod>` | Shows each option's value, default and source: `file`, or `default` when the file gives no usable value (missing, not allowed, or the file is broken). |
| `config <mod> <key> <value>` | Sets an option as the API does: `config blaster radius 16`, `config blaster mode ice`. |
| `config reload <mod>` | Re-reads the player's file after editing it while the game runs. |

## Not covered yet

Syncing values between a client and a server, a generated settings window in the overlay, and lists or tables as
option types. Each can be added later without changing either file format.
