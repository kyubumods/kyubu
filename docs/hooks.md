# Symbols and hooks

This page covers the symbol maps that tell Kyubu where game functions are, and the hook API that lets mods run code inside them. The complete example at the end is the `hook_sample` mod from `mods-examples/`; it builds with the SDK and runs in game.

## Symbol maps

Cube World ships without debug symbols, so Kyubu keeps a hand-made map per game build in `kyubu/symbols/<build id>.toml`. The loader picks the map whose `build` matches the running executable:

| Build id | Executable |
|---|---|
| `cube-0x51D2CE9C` | `Cube.exe` v0.1.1 (PE timestamp `0x51D2CE9C`) |
| `server-0x51D2CE9B` | `Server.exe` v0.1.1 |

A map is a list of `[[symbol]]` tables:

```toml
build = "cube-0x51D2CE9C"

[[symbol]]
name = "Database::get"
address = 0x449D90                  # VA at the preferred base 0x400000 (or: rva = 0x49D90)
signature = "55 8B EC 6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 83 EC 5C"
convention = "thiscall"             # cdecl | stdcall | thiscall | fastcall (documentation)
status = "verified"                 # verified | candidate
notes = "bool(const std::string& key, void** out, uint32_t* len)"
```

| Key | Meaning |
|---|---|
| `name` | Unique name. Loader symbols use `Class::method`; mod symbols must start with `<mod id>:`. |
| `address` / `rva` | Location. `address` is relative to the preferred image base `0x400000`; Kyubu relocates it (the game uses ASLR). |
| `signature` | Bytes expected at the address, `??` for wildcards. Without an address, Kyubu scans `.text` for a **unique** match. |
| `convention` | Calling convention, for readers and tools. |
| `status` | `verified`: confirmed by reverse engineering and testing. `candidate`: a lead that has not been confirmed. Loader features only use verified symbols; mods may use either, at their own risk. |
| `notes` | Prototype and evidence. |

**Verification.** Every symbol is checked the first time it is used: the address must lie in `.text` and the signature must match the file's original bytes (relocated dwords and bytes changed by earlier hooks are ignored, so a symbol still verifies after another mod hooked it). A symbol that fails is **unavailable**: `resolve_symbol` returns `KY_E_UNAVAILABLE` with the reason, and loader features that need it are switched off (their events report "unavailable") instead of patching the wrong code.

**Globals.** Game variables are listed in a separate `[[global]]` array of the same file (or as a `[[symbol]]` with `kind = "global"`), with `name`, `address`, `status` and optionally `type`. A global has no signature: it verifies when its address lies inside the game image. `resolve_symbol` returns the variable's address (for `cube::game_controller`, the address of the pointer, not the controller), and `hook_symbol` refuses a global. Loader 1.0.1 reads only `[[symbol]]`, so it skips the array.

**Symbols in mods.** A mod can ship its own maps in `symbols/*.toml` inside its package (the folder name can be changed with `symbols = "..."` in `mod.toml`). Names must be prefixed with the mod id. They resolve like loader symbols, for that mod and for any other mod.

**Tooling.** The maps are generated: the source of truth is the mapping database in `mapping/` (names, prototypes, types, status and evidence for both builds), and `kmap export` writes `symbols/<build>.toml` from it, with signatures extended until they are unique in `.text`. Never edit a map by hand; `kmap check` fails when a map differs from what the database produces. See [mapping/setup.md](mapping/setup.md) for the workspace and `mapping/conventions.md` for the rules. `tools/symgen.py` still checks a map against an executable (`check`) and looks symbols up in a different build by signature (`find`, output marked `candidate`).

**Renamed symbols keep working.** When an entry gets a better name (for example `GameController::update` became `cube::GameController::update` once RTTI named the class), the old name stays in the map as an extra entry at the same address, with `notes = "deprecated alias of ..."`, so mods that resolve it are not affected. Every name that shipped in a release is listed in `mapping/exported-names.lock`, and `kmap check` fails if one of them stops resolving.

The console command `sym <name>` shows a symbol's address, status and verification result (`sym` alone lists all of them).

### Symbols in this release

The names the loader itself uses are listed below. Every exported name, with its prototype, addresses, status and the probes that confirm it, is in the game reference, [game/README.md](game/README.md), which `kmap export` generates from `mapping/`. The same names are in the C headers (`sdk/include/cubeworld/`, `CW_SYM_<name>` for `resolve_symbol`) and in the Lua module (`require("cubeworld")`, see [lua.md](lua.md)).

Names in parentheses are the older names, which still resolve.

`Cube.exe`, verified: `WinMainCRTStartup` (`entry`), `WinMain`, `Game::frame`, `cube::GameController::{GameController, update, render}` (`GameController::...`), `cube::Database::{Database, ~Database, open, get, getBuffer}` (`Database::...`), `asset_decode`, `sqlite3_close`, `ModelLoader::{loadAll, resizeSite}`, `ModelVector::{resize, at}`, `Model::{Model, load}`, `Net::connectPortSite`, `cube::World::openSave` (`World::openSave`), `cube::WorldMap::openSave` (`Map::openSave`). Candidates: `matrix_transform_normal` (`ModelResolverB`) and `cube::World::generateZone` (`ModelResolverC`); task 8.4 of add-mapping-workspace found that neither resolves model slots (see `mapping/atlas/creatures.md`).

`Server.exe`, verified: `mainCRTStartup` (`entry`), `main`, `run_server` (`Server::main`), `cube::Database::{Database, open, get, getBuffer}` (`Database::...`), `asset_decode`, `sqlite3_close`, `cube::World::openSave` (`World::openSave`), `connection_send_loop` (`Server::simLoop`), `connection_send_pass_site` (`Server::tickSite`). Candidates: `ModelLoader::loadAll`, `ModelVector::resize`, `Model::load`. `Server::simLoop` and `Server::tickSite` kept their 1.0.1 names as aliases, but they are one client's send loop and its once-per-pass site, not the server's tick.

Added for multiplayer abilities (openspec `add-skills-unleashed-multiplayer`, section 10), all verified on both exes unless named:

- World step and hits ([`sim.world_step`](events.md), [`creature_hit`](game-api.md#creature_hit)): `cube::World::worldStepSite`, `cube::World::hitCreature`, `cube::World::hitListSite`.
- Rooting and movement ([`creature_set_flags`](game-api.md#creature_set_flags) `rooted`, [`creature_state_set`](game-api.md#creature_state_set)): `cube::World::aiTickSite`, `cube::World::moveStepSite`.
- Channels on a joined game ([sides-and-networking.md](sides-and-networking.md), "Joined games"): `Net::clientReceiveNextSite` and `Net::clientSendPassSite` (`Cube.exe`), `Net::connectionReceiveNextSite` and `connection_send_pass_site` (`Server.exe`).
- Player shots ([`shot_take`](game-api.md#shot_take), [`projectile_push`](game-api.md#projectile_push), `Server.exe` only): `Net::receiveProjectileSite`, `std_list_projectile_push_back`.
- The join gate: `Net::connectVersionSite` (`Cube.exe`) and `Net::acceptVersionSite` (`Server.exe`), candidates.

Most of these sites carry the loader's own mid hooks; use the API calls linked above before hooking them yourself.

## Hook kinds

| Kind | API (native) | Lua | Use it for |
|---|---|---|---|
| Function hook | `hook_function(addr)`, `hook_symbol(name)` | `kyubu.hook(target, ctype, fn)` | Replacing or wrapping a whole function. |
| Import hook | `hook_import(dll, function)` | `kyubu.hook_import(...)` | Windows API calls made by the game (`kernel32`, `ws2_32`, `d3d9`...). |
| Virtual method hook | `hook_vtable(object, index)` | `kyubu.hook_vtable(...)` | COM interfaces (Direct3D, DirectInput) and C++ virtual methods. |
| Mid-function hook | `hook_mid(address, fn)` | `kyubu.hook_mid(target, fn)` | Reading or changing registers at one instruction. |
| Byte patch | `patch_bytes(address, bytes)` | `kyubu.patch(addr, bytes)` | Changing constants or jumps. |

All of them return a hook id; `unhook(id)` removes one, and everything a mod installed is removed when it is unloaded or faults.

### Function, import and vtable hooks: chains

Several mods can hook the same function. Kyubu keeps one chain per target and patches the target only once:

- Each hook gets a **next** pointer (`void** next_slot`) that it must call to continue the chain; the last hook's next is the original function.
- Order: higher `priority` runs first (outermost); equal priorities follow the mod load order.
- Removing a hook from the middle relinks the chain; the others keep working. The target is unpatched when the last hook is removed.
- A handler must have **exactly** the target's signature and calling convention.

Not calling `next` replaces the function for every hook behind yours; do that only on purpose.

### Calling conventions and `__thiscall`

Most game functions are C++ member functions (`__thiscall`: `this` in ECX, other arguments on the stack, callee pops). GCC accepts `__thiscall` on free functions, MSVC does not. The portable way is a `__fastcall` function with an unused second parameter: `__fastcall` passes the first two arguments in ECX and EDX, the rest on the stack, and the callee pops, so

```cpp
bool __fastcall hook(void* self, void* edx_unused, const void* key, void** out, uint32_t* len);
```

has the same ABI as `bool __thiscall Database::get(const std::string&, void**, uint32_t*)`. Declare the next pointer with the same `__fastcall` type and pass `edx_unused` through.

In Lua, FFI function types carry the convention: `"bool(__thiscall*)(void*, void*, void**, uint32_t*)"`. The handler receives `next` as its first argument.

### Mid-function hooks

`hook_mid(address, fn)` runs `fn(KyubuCpuContext* ctx, user)` when execution reaches `address`. `ctx` holds EAX-EDI, EBP, EFLAGS, XMM0-XMM7 (`ctx->xmm[n].f32[lane]` or `.u32[lane]`, 1.1), plus the site's ESP and EIP (read-only). Register changes are written back when `fn` returns, the XMM ones too. A mod that edits XMM registers requires `kyubu = "^1.1"`: it is how the game's float code (SSE: `movss`, `cvtdq2ps`, `mulss`) is changed at an instruction. Several mods can hook the same site; their callbacks run in the order they were installed.

Kyubu relocates the instructions it overwrites (at least 5 bytes). It **refuses** (`KY_E_UNSAFE`) a site whose displaced instructions include a relative `call`, `jmp` or conditional jump, or that it cannot decode, because moving them would change their target. Pick an instruction a few bytes earlier or later.

Kyubu does **not** check whether other code jumps into the bytes it overwrites. If a jump, a `call` or a switch's jump table lands on the second or third instruction of the 5-byte patch, the game runs the middle of the `jmp` and crashes (an access violation at the site + 2 or + 3; seen at two sites in the probe `ability-walk-speed`, 2026-10-19). A site whose own instruction is 5 bytes or longer is safe from this. For a shorter one, check in the disassembly that no jump targets the instructions that follow it within 5 bytes, and that no table holds their addresses.

### Byte patches

`patch_bytes(address, bytes, len)` writes bytes and remembers the originals; `unhook` restores them. Patches and hooks record which bytes they own; a patch that overlaps bytes owned by **another** mod fails with `KY_E_CONFLICT`, naming the owner, and nothing is written.

### Safety rules

- Code is written with the other game threads suspended, so no thread executes half-written instructions.
- Chained hooks are switched atomically: the chain is rebuilt tail-first before the head changes.
- Trampolines are never freed while the process runs, so a thread that is still inside a removed hook cannot crash.
- Event handlers and mid hooks run inside a fault guard: a crash in them marks the mod **faulted**, removes all its hooks and keeps the game running. Function, import and vtable hooks are called directly by the game for speed, so a crash there is caught by the crash handler, which names the mod whose code faulted.

## Reading and writing memory

| Function | Notes |
|---|---|
| `mem_read(addr, out, len)` | Guarded: returns `KY_E_ACCESS` instead of crashing on a bad address. |
| `mem_write(self, addr, data, len)` | Guarded; changes page protection when needed. |
| `game_alloc(size)` / `game_free(p)` | The game's own `operator new`/`delete` (MSVC 2012 CRT). Use them for memory the game will free, or that you hand to game code; never mix them with your CRT's allocator. |
| `rva_to_addr(rva)` / `addr_to_rva(addr)` | Convert between RVAs and runtime addresses. |

## Example: `hook_sample`

The mod hooks `Database::get` by symbol (a `__thiscall` member function), `kernel32!Sleep` in Cube.exe's import table, and the instruction that sizes the model table (a mid hook). It also ships a symbol of its own and registers a console command, `hook_sample:stats`, that prints the call counts.

`mods-examples/hook_sample/mod.toml`:

<!-- sample: mods-examples/hook_sample/mod.toml -->
```toml
id = "hook_sample"
name = "Hook sample"
version = "1.0.0"
kyubu = "^1.0"
sides = ["client"]
description = "The example from docs/hooks.md: a function hook, an import hook, a mid-function hook and a mod-defined symbol."

[native]
client = "bin/hook_sample.dll"
```

`mods-examples/hook_sample/symbols/hook_sample.toml`:

<!-- sample: mods-examples/hook_sample/symbols/hook_sample.toml -->
```toml
# Symbols defined by this mod. Names must start with "hook_sample:".
build = "cube-0x51EA955E"

[[symbol]]
name = "hook_sample:ModelLoader::loadAll"
address = 0x4EE230
signature = "55 8B EC 6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00"
convention = "thiscall"
status = "candidate"
notes = "same function as the loader's ModelLoader::loadAll; shows how a mod ships its own symbols"
```

`mods-examples/hook_sample/src/main.cpp`:

<!-- sample: mods-examples/hook_sample/src/main.cpp -->
```cpp
// hook_sample: the example from docs/hooks.md.
// Builds with MinGW (i686) and MSVC (Win32). Every hook here is removed automatically when the game exits.
#include <cstdint>
#include <cstdio>

#include "kyubu.hpp"

namespace {

kyubu::Mod* g_mod = nullptr;

// 1. A __thiscall member function, hooked by symbol name.
//    bool Database::get(const std::string& key, void** out, uint32_t* len)
//    MSVC cannot declare free __thiscall functions, so the handler is a __fastcall with an unused EDX
//    parameter: ECX = this, EDX = unused, the rest on the stack, callee pops - the same ABI as __thiscall.
using DbGetFn = bool(__fastcall*)(void* self, void* edx, const void* key, void** out, uint32_t* len);
DbGetFn g_db_get_next = nullptr;
unsigned g_db_reads = 0;

bool __fastcall db_get_hook(void* self, void* edx, const void* key, void** out, uint32_t* len) {
    ++g_db_reads;
    return g_db_get_next(self, edx, key, out, len);   // always call next unless you replace the function
}

// 2. An imported Windows function, hooked in Cube.exe's import table.
using SleepFn = void(__stdcall*)(uint32_t ms);
SleepFn g_sleep_next = nullptr;
unsigned g_sleeps = 0;

void __stdcall sleep_hook(uint32_t ms) {
    ++g_sleeps;
    g_sleep_next(ms);
}

// 3. A mid-function hook: runs at one instruction with the CPU registers in `ctx`.
//    ModelLoader::resizeSite is the `push 0xA09` that sizes the model table (see docs/registries.md).
void KYUBU_CALL model_table_site(KyubuCpuContext* ctx, void*) {
    g_mod->info("model table sized at 0x%08X (esp=0x%08X)", ctx->eip, ctx->esp);
}

void KYUBU_CALL cmd_stats(const char*, KyubuPrintFn print, void* pctx, void*) {
    char buf[128];
    std::snprintf(buf, sizeof buf, "Database::get calls: %u, Sleep calls: %u", g_db_reads, g_sleeps);
    print(pctx, buf);
}

}  // namespace

struct HookSample {
    explicit HookSample(kyubu::Mod& m) : mod(m) {
        g_mod = &m;
        const KyubuApi* api = m.api();

        if (!mod.hook("Database::get", &db_get_hook, &g_db_get_next))
            mod.warn("Database::get is not available on this build");

        KyubuId id = 0;
        if (api->hook_import(m.handle(), "kernel32.dll", "Sleep", (void*)&sleep_hook, 0, (void**)&g_sleep_next, &id) != KY_OK)
            mod.warn("could not hook Sleep");

        // A mod-defined symbol from symbols/hook_sample.toml resolves like a loader symbol.
        if (uintptr_t a = mod.symbol("hook_sample:ModelLoader::loadAll"))
            mod.info("hook_sample:ModelLoader::loadAll resolved to 0x%08X", (unsigned)a);

        uintptr_t site = mod.symbol("ModelLoader::resizeSite");
        if (site && api->hook_mid(m.handle(), site, model_table_site, nullptr, &id) != KY_OK)
            mod.warn("mid hook refused (see the log for the reason)");

        api->register_command(m.handle(), "hook_sample:stats", "show hook call counts", cmd_stats, nullptr);
    }
    kyubu::Mod& mod;
};

KYUBU_MOD("hook_sample", KY_SIDE_CLIENT, HookSample)
```

Build it with the SDK template's CMake file (see [native-api.md](native-api.md)) or as part of this repository (`cmake --build build --target hook_sample`), copy `build/mods/hook_sample` into `mods/`, start the game and type `hook_sample:stats` in the console:

```
> hook_sample:stats
Database::get calls: 2672, Sleep calls: 3175
```
