#pragma once
#include "common.h"

#include "modmanager.h"

namespace kyubu {

// The mod-management permission (API 1.2; add-mod-manager, design.md "Permission"): the members of
// the management section call this and refuse unless the calling mod is full trust AND its mod.toml
// declares manage_mods = true. Returns KY_OK, or KY_E_DENIED with a refusal logged once per
// (mod, function): "restricted mods cannot manage mods" for a mod of another trust, or the missing
// manage_mods key for a full-trust mod without it.
KyubuResult manage_permission(const Mod* mod, KyubuSide side, const char* fn);

// The listing (mods_count / mods_get): every package discover_and_resolve found in mods\ at boot, whatever its
// state, in discovery order (no loading, no changes; the API wrappers have checked manage_permission first).
// Listing a package that has no Package yet opens it, so zip state and sizes are available without the loader
// having opened anything at discover time.
uint32_t manage_mods_count();
KyubuResult manage_mods_get(uint32_t index, KyubuModInfo* out);

// mods_package_hash: the SHA-256 of the package of `id` (D5). A zip hashes its whole file. A folder hashes a
// canonical listing: each file's relative path with '/', its size and its own SHA-256, sorted by path, one line
// each (`"<path> <size> <hex>\n"`), the SHA-256 of that text; the same rule kcat uses, with the shared test
// vector in tests/fixtures/manage/. Computed on the first request and cached by path, size and modification time.
KyubuResult manage_package_hash(const char* id, uint8_t out[32]);

// The options of any package (design.md "The options of any mod"): the declarations of its
// config.toml and the value in use, for a loaded mod through its own ModConfig, for a disabled, failed or
// faulted package by parsing the declaration on request. Options are counted and read in file order.
uint32_t manage_option_count(const char* id);
KyubuResult manage_option_get(const char* id, uint32_t index, KyubuOptionInfo* out);

// mods_option_check: parses a value given as text and checks it against the declaration, without saving.
KyubuResult manage_option_check(const char* id, const char* key, const char* value);

// mods_option_set (D14): for a loaded mod exactly its own config_set (validation, the single-key rewrite,
// config.changed queued when the value changed); for a package that is not loaded the player's file is written
// (created with every default when it does not exist) and no event is queued.
KyubuResult manage_option_set(const char* id, const char* key, const char* value);

// mods_rescan (D15): rereads mods\ (the discover rules: a folder or a zip with a mod.toml). A package added
// since boot joins the listing as `new` (state `failed` with the parse error when its mod.toml does not
// parse); a discovered package whose path is gone is listed as `missing`. Nothing is loaded, unloaded or
// changed, and the listing order keeps the boot order (then the new ones).
KyubuResult manage_rescan();

// The queue (D2): the changes mod_manager wants at the next boot. Every call that changes the queue rewrites
// kyubu\pending.toml in full and touches no other file. install names a package inside kyubu\staging (or
// kyubu\backup, for an undo) and the id and version it must contain; config is checked against the
// declaration now, from the installed package or from a staged install of the same id in the queue.
KyubuResult manage_queue(const KyubuModOp* op);
uint32_t manage_queue_count();
KyubuResult manage_queue_get(uint32_t index, KyubuModOp* out);
KyubuResult manage_queue_clear();

// What applying the pending file came to (D2): how many ops ran and failed, and whether safe mode kept it.
struct PendingResult {
    uint32_t applied = 0;
    uint32_t failed = 0;
    bool skipped = false;
};

// Applies kyubu\pending.toml in the spec's order (installs, enable and disable, deletes, options), one log
// line per failed op and the rest still applied, then deletes the file. Before the first op it writes
// kyubu\backup\last-apply.toml (the previous [mods], the previous options files, the packages it moves) and
// deletes staged files that no op names. Safe mode keeps the file and applies nothing. A boot with no pending
// file purges the packages last-apply.toml records (kyubu\backup\zlib1.dll and the rest stay).
PendingResult apply_pending();

// The one notice the start menu shows after a failed apply ("" when none failed): the spec's
// "ModManager: 1 of 5 changes failed, see the log".
std::string pending_notice(const PendingResult& r);

}  // namespace kyubu
