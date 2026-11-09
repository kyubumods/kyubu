# Native mods (C and C++)

Native mods are 32-bit Windows DLLs that talk to Kyubu through one C function table, `KyubuApi`, declared in `sdk/include/kyubu_api.h`. `sdk/include/kyubu.hpp` is an optional header-only C++20 layer on top of it. Native mods have full access to the process: they can hook anything and read or write any memory.

## The SDK

| Path | Contents |
|---|---|
| `include/kyubu_api.h` | The C ABI (C99 and C++). |
| `include/kyubu.hpp` | C++20 helpers: `kyubu::Mod`, typed hooks, lambdas as event handlers, the `KYUBU_MOD` macro. |
| `include/cubeworld/` | The game's types, function-pointer types and symbol names (`CW_SYM_<name>` for `resolve_symbol`), generated from the mapping database; `#include <cubeworld/cubeworld.h>`. No addresses: those come from the symbol maps. Every entry is described in [game/README.md](game/README.md). `cubeworld/layout.h` (not included by `cubeworld.h`) lists every mapped type's fields, offsets and sizes as tables, for tools such as the probe inspector. |
| `include/kyubu_probe.h`, `dev-mods/` | The `kyubu_probe` developer mod and its finder interface; see [probes.md](probes.md). |
| `templates/native-mod/` | CMake project that builds and stages a mod. |
| `templates/lua-mod/` | Lua mod skeleton with editor type stubs. |
| `lua/kyubu.lua`, `lua/cubeworld.lua` | LuaLS type stubs for the `kyubu` API and for the game types (`require("cubeworld")`, see [lua.md](lua.md)). |
| `ghidra/apply.py`, `ghidra/mappings.json` | Names, prototypes and types for your own Ghidra project of `Cube.exe` or `Server.exe`: open the program and run `apply.py` (usage at the top of the script). It checks the program's SHA-256 first and changes nothing on another build. |
| `cmake/mingw-i686.cmake` | Toolchain file for cross-compiling with MinGW-w64 on Linux. |
| `tools/` | `cwmod` (asset pipeline) and `symgen.py` (symbol maps). |
| `docs/` | This documentation. |

## Building

The game is 32-bit, so mods must be too. Any compiler that produces Win32 DLLs works, because nothing but plain C crosses the ABI. The template supports:

```
# Visual Studio 2019/2022 (x86)
cmake -B build -A Win32
cmake --build build --config Release

# MinGW-w64 on Windows (i686)
cmake -B build -G "MinGW Makefiles"
cmake --build build

# MinGW-w64 cross-compiler on Linux
cmake -B build -DCMAKE_TOOLCHAIN_FILE=/absolute/path/to/sdk/cmake/mingw-i686.cmake   # must be an absolute path
cmake --build build
```

The result is staged in `build/my_mod/` (`mod.toml`, `bin/my_mod.dll`, `assets/`): copy that folder into the game's `mods\`. Link the C and C++ runtimes statically (the template does), so the mod does not need a redistributable. The game uses the Visual C++ 2012 runtime; never free memory from one runtime with another (see [Memory](#memory)).

## Entry points

A native mod exports two functions and optionally a third, all `cdecl`:

```c
KYUBU_EXPORT int  KYUBU_CALL kyubu_mod_query(KyubuModQuery* q);   /* fill api_version, id, sides */
KYUBU_EXPORT int  KYUBU_CALL kyubu_mod_init(const KyubuApi* api, KyubuModHandle self, KyubuSide side);
KYUBU_EXPORT void KYUBU_CALL kyubu_mod_shutdown(KyubuSide side);  /* optional */
```

1. `kyubu_mod_query` runs first. The mod reports the API version it was built against, its id (must equal `mod.toml`) and the sides it has code for. A mismatch fails the mod with a clear reason.
2. `kyubu_mod_init` runs during PreInit, **once per logical side** the mod handles (twice in single-player). Each call gets its own `KyubuModHandle`; pass it to every API call made on behalf of that side. Return 0 for success; anything else fails the mod.
3. `kyubu_mod_shutdown(side)` runs at exit, once per side, after `lifecycle.shutdown`.

`[native]` in `mod.toml` names the DLL: `common` (one DLL for every side), or separate `client` / `server` DLLs.

With `kyubu.hpp`, `KYUBU_MOD("id", sides, Type)` generates all three: it constructs `Type(kyubu::Mod&)` in `init` and destroys it in `shutdown`, once per side.

## Template

`sdk/templates/native-mod/src/main.cpp`:

<!-- sample: sdk/templates/native-mod/src/main.cpp -->
```cpp
// my_mod: native Kyubu mod template. See docs/native-api.md.
#include "kyubu.hpp"

struct MyMod {
    // Called once per logical side listed in KYUBU_MOD below (twice in single-player).
    explicit MyMod(kyubu::Mod& m) : mod(m) {
        mod.info("my_mod loaded on the %s side", m.side() == KY_SIDE_CLIENT ? "client" : "server");

        // Registries are shared by both sides of a process; adding the same entry on each side is fine.
        mod.on("lifecycle.register", [this](KyubuEvent&) {
            mod.add_model("my_mod:cube", "my_mod-cube.cub");   // assets/models/my_mod-cube.vox
        });
        mod.on("lifecycle.game_ready", [this](KyubuEvent&) {
            mod.info("my_mod:cube is model slot %u", mod.model_slot("my_mod:cube"));
        });

        if (m.side() == KY_SIDE_CLIENT) {
            mod.on("input.key", [this](KyubuEvent& ev) {
                auto* k = kyubu::Mod::data<KyubuKeyData>(ev);
                // an option from config.toml; players change it in kyubu/config/my_mod.toml
                if (k->vk == 0x77 /* F8 */ && k->down && mod.config_bool("log_f8", true)) mod.info("F8 pressed");
            });
        }
    }
    // Called on shutdown (once per side).
    ~MyMod() { mod.info("my_mod shutting down"); }

    kyubu::Mod& mod;
};

KYUBU_MOD("my_mod", KY_SIDE_CLIENT | KY_SIDE_SERVER, MyMod)
```

The same in plain C (this file is compiled as strict C99 by the SDK header checks, `tests/sdk/check_headers.sh`):

<!-- sample: tests/sdk/c99_check.c -->
```c
/* Compiles kyubu_api.h as strict C99 and exercises the entry-point declarations. */
#include "kyubu_api.h"

KYUBU_EXPORT int KYUBU_CALL kyubu_mod_query(KyubuModQuery* q) {
    q->api_version = KYUBU_API_VERSION;
    q->id[0] = 'c'; q->id[1] = '9'; q->id[2] = 0;
    q->sides = KY_SIDE_CLIENT;
    return 0;
}

static void KYUBU_CALL on_frame(KyubuEvent* ev, void* user) { (void)ev; (void)user; }

KYUBU_EXPORT int KYUBU_CALL kyubu_mod_init(const KyubuApi* api, KyubuModHandle self, KyubuSide side) {
    KyubuId id = 0;
    (void)side;
    if (api->size < sizeof(KyubuApi)) return -1;
    return api->subscribe(self, "frame.begin", KY_SIDE_CLIENT, 0, on_frame, 0, &id);
}
```

## ABI rules

- 32-bit x86, `cdecl` for every function pointer in the table and every callback.
- The table only grows within major version 1. `api->size` is the size of the table the loader has: check it before using a member added after 1.0 (`offsetof(KyubuApi, member) + sizeof(void*) <= api->size`).
- No C++ types, exceptions or loader-owned heap memory cross the boundary. Strings are UTF-8 and NUL-terminated. Output buffers belong to the caller; when one is too small the function returns `KY_E_BUFFER` and, where it has an `out_len`, the size needed.
- Handles (`KyubuModHandle`, `KyubuId`) are opaque. A handle identifies the mod **and** the side.
- Don't let C++ exceptions escape a callback.

## Result codes

| Code | Value | Meaning |
|---|---|---|
| `KY_OK` | 0 | Success. |
| `KY_E_INVALID_ARG` | -1 | Bad argument (null pointer, malformed name, unknown handle). |
| `KY_E_PHASE` | -2 | Not allowed in the current lifecycle phase (see [mods.md](mods.md)). |
| `KY_E_SIDE` | -3 | Not allowed from this logical side (see [sides-and-networking.md](sides-and-networking.md)). |
| `KY_E_NOT_FOUND` | -4 | The named thing does not exist. |
| `KY_E_UNAVAILABLE` | -5 | Needs a symbol or feature this game build does not have. |
| `KY_E_CONFLICT` | -6 | Overlaps something another mod owns (patches, hook sites). |
| `KY_E_UNSAFE` | -7 | Refused because it cannot be done safely (e.g. a mid hook on a relative jump). |
| `KY_E_ACCESS` | -8 | Invalid memory address. |
| `KY_E_NAMESPACE` | -9 | Identifier outside the caller's `<mod id>:` namespace. |
| `KY_E_DUPLICATE` | -10 | Identifier already registered. |
| `KY_E_TOO_LARGE` | -11 | Payload over a limit (channels: 64 KiB). |
| `KY_E_VERSION` | -12 | Version requirement not met (e.g. interface version). |
| `KY_E_INTERNAL` | -13 | Loader bug; see the log. |
| `KY_E_BUFFER` | -14 | Output buffer too small. |
| `KY_E_NO_REMOTE` | -15 | The remote end has no Kyubu (channels on vanilla servers). |
| `KY_E_FAULTED` | -16 | The calling mod was disabled after a fault. |
| `KY_E_DEV_ONLY` | -17 | Needs dev mode. |
| `KY_E_TRUST` | -18 | Not permitted at the mod's trust level. |
| `KY_E_GONE` | -19 | 1.1: the creature a handle named no longer exists ([game-api.md](game-api.md)). |
| `KY_E_REFUSED` | -20 | 1.1: the game state refuses the request, for example `ability_start` on a cooldown ([game-api.md](game-api.md)). |

Failures that have a reason (a signature mismatch, a conflict with another mod) are also written to the log under the calling mod's name.

## The API table at a glance

| Group | Functions | Docs |
|---|---|---|
| Core | `log`, `phase`, `current_side`, `host_info`, `mod_id`, `game_dir`, `mod_data_dir`, `read_package_file`, `is_mod_loaded`, `buffer_alloc`, `buffer_free`, `run_on_main_thread` | below |
| Symbols and memory | `resolve_symbol`, `rva_to_addr`, `addr_to_rva`, `mem_read`, `mem_write`, `game_alloc`, `game_free` | [hooks.md](hooks.md) |
| Hooks | `hook_function`, `hook_symbol`, `hook_mid`, `hook_import`, `hook_vtable`, `patch_bytes`, `unhook` | [hooks.md](hooks.md) |
| Events | `subscribe`, `unsubscribe`, `emit`, `event_cancel` | [events.md](events.md) |
| Assets | `asset_provider`, `asset_read`, `asset_served_by` | [assets.md](assets.md) |
| Registries | `registry_create`, `registry_add`, `registry_remap`, `registry_lookup`, `registry_name`, `registry_payload`, `registry_count`, `registry_entry`, `registry_hash` | [registries.md](registries.md) |
| Channels | `channel_register`, `channel_send`, `session_manifest` | [sides-and-networking.md](sides-and-networking.md) |
| Interfaces | `publish_interface`, `get_interface` | below |
| Console and UI (client) | `register_command`, `imgui_context`, `ui_begin` ... `ui_want_input` | [dev-tools.md](dev-tools.md) |
| Diagnostics | `mod_stats`, `mod_state` | [dev-tools.md](dev-tools.md) |
| Game API (1.1) | `game_available` | [game-api.md](game-api.md) |
| Abilities (1.1) | `ability_define`, `ability_number`, `ability_name`, `ability_list`, `ability_slot_owner` | [registries.md](registries.md), [abilities.md](abilities.md) |
| Abilities in play (1.1) | `ability_get`, `ability_cooldown`, `ability_start`, `ability_end`, `ability_rank_get`, `ability_rank_set` | [game-api.md](game-api.md) |
| Animation binding (1.1) | `ability_animation_bind` | [game-api.md](game-api.md) |
| UI scene and render device (1.1) | `ui_node_find`, `ui_node_get`, `ui_node_child`, `render_get` | [game-api.md](game-api.md) |
| Animation editor (1.1, dev mode) | `animation_define_from_text`, `animation_preview`, `animation_preview_clear` | [game-api.md](game-api.md) |
| Options (1.1) | `config_get_bool`, `config_get_int`, `config_get_float`, `config_get_string`, `config_set` | [config.md](config.md) |

### Threads

Callbacks run on the game's main thread unless documented otherwise (network events can run on the socket thread). The API is **not thread-safe**, except `log`, `channel_send` and `run_on_main_thread(self, fn, user)`, which queues `fn` to run on the main thread at the start of the next frame.

### Memory

- `game_alloc` / `game_free` use the game's own allocator (the VC++ 2012 `operator new`/`delete`). Use them for anything the game will free or that you store in game objects.
- `buffer_alloc` / `buffer_free` are for asset-provider results: the provider allocates, the loader frees.
- Everything else you allocate with your own runtime and free yourself.

### Faults

Every event handler, mid hook, command, provider, channel handler and entry point runs inside a structured-exception guard. When one crashes, Kyubu logs the exception and address, marks the mod **faulted**, removes its hooks and patches, stops calling its handlers, commands and providers, and the game keeps running. Function, import and vtable hooks are called directly by the game, without a guard; a crash there reaches the crash handler, which writes a report naming the mod whose code crashed.

### Mod-to-mod interfaces

A mod can publish a pointer under a name in its namespace, usually a struct of function pointers, and other mods can look it up:

```cpp
struct CounterV1 { int (*frames)(); };
static CounterV1 g_counter{ [] { return g_frames; } };
api->publish_interface(self, "hello_native:counter", 1, &g_counter);      // publisher

void* p; uint32_t version;
if (api->get_interface("hello_native:counter", 1, &p, &version) == KY_OK) {   // consumer, any version >= 1
    int n = static_cast<CounterV1*>(p)->frames();
}
```

Declare the publisher as a dependency (or optional dependency) so it loads first. Keep interfaces C-compatible and only ever append to them; bump the version when you do.

### Logging

`api->log(self, level, message)` writes `timestamp LEVEL [mod id] [side] message` to `kyubu\logs\latest.log`. `kyubu::Mod` adds printf-style `info`, `warn`, `error`.

## Examples

- `sdk/templates/native-mod`: the template above.
- `mods-examples/hello_native`: events, a `Database::get` hook, a console command, an interface and a new model.
- `mods-examples/hook_sample`: every hook kind; walked through in [hooks.md](hooks.md).
