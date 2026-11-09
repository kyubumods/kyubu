// The mod-management permission: the check every mod-management member (API 1.2) runs first.
// The listing, the package hash and the options of any mod (openspec add-mod-manager, design D5, D14, D15).
#include "manage.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

#include "config.h"
#include "loader.h"
#include "log.h"
#include "modconfig.h"
#include "semver.h"
#include "sha256.h"
#include "toml_config.h"

namespace kyubu {

namespace fs = std::filesystem;

namespace {

// Copies a string into a char array. True when it fits.
bool put(char* dst, size_t cap, const std::string& s) {
    if (s.size() >= cap) return false;
    std::memcpy(dst, s.c_str(), s.size() + 1);
    return true;
}

// The package's mtime stamp for the cache: of the zip file, or of the folder itself.
uint64_t stamp_of(const std::string& path) {
    std::error_code ec;
    auto t = fs::last_write_time(fs::path(widen(path)), ec);
    if (ec) return 0;
    return (uint64_t)t.time_since_epoch().count();
}

// The package's size: the zip file, or every file of the folder.
uint64_t size_of(Package& pkg) {
    if (pkg.is_zip()) {
        std::error_code ec;
        auto n = fs::file_size(fs::path(widen(pkg.path())), ec);
        return ec ? 0 : n;
    }
    uint64_t total = 0;
    for (auto& rel : pkg.list(""))
        total += [&]() -> uint64_t {
            std::error_code ec;
            auto n = fs::file_size(fs::path(widen(join_path(pkg.path(), rel))), ec);
            return ec ? 0 : n;
        }();
    return total;
}

void fill_dep(const Dependency& d, KyubuModDep* out, const std::string& other_id) {
    put(out->id, sizeof out->id, other_id);
    put(out->req, sizeof out->req, d.req.text());
    out->optional = d.optional ? 1u : 0u;
}

// a dependency entry names the mod it needs; a dependent entry names the mod that needs it
void fill_dep(const Dependency& d, KyubuModDep* out) {
    fill_dep(d, out, d.id);
}

}  // namespace

KyubuResult manage_permission(const Mod* mod, KyubuSide side, const char* fn) {
    if (mod && mod->manifest.trust == "full" && mod->manifest.manage_mods) return KY_OK;

    const bool restricted = !mod || mod->manifest.trust != "full";
    const std::string why = restricted ? "restricted mods cannot manage mods"   // spec: the log says this once
                                       : "mod management needs manage_mods = true in mod.toml";
    static std::mutex mu;
    static std::set<std::string> seen;
    std::string key = (mod ? mod->id : std::string("?")) + "/" + fn;
    {
        std::lock_guard<std::mutex> g(mu);
        if (seen.insert(key).second)
            Log::get().write(KY_LOG_WARN, mod ? mod->id : std::string("kyubu"), side,
                             std::string(fn) + ": " + why);
    }
    return KY_E_DENIED;
}

// The options of any mod, the decision of the rescan state: helpers used by both
namespace {

// Discovery opens nothing; the listing, the hash and the options each need the package (zip state, sizes,
// the files). One lock and one open, used by all of them.
bool open_pkg(Mod* m) {
    if (!m->pkg) {
        static std::mutex mu;
        std::lock_guard<std::mutex> g(mu);
        if (!m->pkg) m->pkg = Package::open(m->path);
    }
    return m->pkg != nullptr;
}

// `id` gives one Mod of the listing, whatever its state (a disabled or failed package keeps its record).
Mod* find_package(const std::string& id) {
    for (auto& m : ModManager::get().all())
        if (m->id == id) return m.get();
    return nullptr;
}

// Manifest parsing failures are per id (the same text the boot log shows); one warning is enough for them.
void log_decl_error_once(const std::string& id, const std::string& what) {
    static std::mutex mu;
    static std::set<std::string> seen;
    std::lock_guard<std::mutex> g(mu);
    if (seen.insert(id).second)
        Log::get().write(KY_LOG_WARN, id, KY_SIDE_NONE, "options: " + what);
}

std::string joined(const std::vector<std::string>& parts) {
    std::string all;
    for (auto& e : parts) all += (all.empty() ? "" : "; ") + e;
    return all;
}

// What a package's config.toml gave a request: no declaration, a parsed one, or one that does not parse.
enum class DeclStatus { None, Ok, Bad };

}  // namespace

// ----------------------------------------------------------------- the listing
std::mutex g_rescan_mu;
std::vector<std::unique_ptr<Mod>> g_rescan_added;   // packages that mods_rescan found after boot, in path order

uint32_t manage_mods_count() {
    std::lock_guard<std::mutex> g(g_rescan_mu);
    return (uint32_t)(ModManager::get().all().size() + g_rescan_added.size());
}

namespace {

// Slower than a memcpy but small: a description or joined list clipped by the header entry.
bool put_clipped(char* dst, size_t cap, const std::string& s) {
    // copy_out truncates and returns KY_E_BUFFER; here an empty tail is fine for a description, not for the rest
    if (s.size() >= cap) {
        std::memcpy(dst, s.c_str(), cap - 1);
        dst[cap - 1] = 0;
        return true;
    }
    std::memcpy(dst, s.c_str(), s.size() + 1);
    return true;
}

void fill_info(const Mod* m, KyubuModInfo* out) {
    // A manifest that does not parse fills none of its text fields ("empty when the manifest does not parse").
    put(out->id, sizeof out->id, m->id);
    if (m->manifest_ok) {
        // Anything that overflows a char entry clips; a 255+ description is not worth an error to the caller.
        put(out->version, sizeof out->version, m->manifest.version.str());
        put(out->kyubu_req, sizeof out->kyubu_req, m->manifest.api_req.text());
        put(out->trust, sizeof out->trust, m->manifest.trust);
        std::string authors;
        for (auto& a : m->manifest.authors) authors += (authors.empty() ? "" : ", ") + a;
        put_clipped(out->authors, sizeof out->authors, authors);
        put_clipped(out->description, sizeof out->description, m->manifest.description);
        put(out->name, sizeof out->name, m->manifest.name);
    }
    put_clipped(out->reason, sizeof out->reason, m->reason);
    put(out->path, sizeof out->path, m->path);
    switch (m->state) {
        case ModState::Loaded: out->state = KY_MOD_STATE_LOADED; break;
        case ModState::Disabled: out->state = KY_MOD_STATE_DISABLED; break;
        case ModState::Failed: out->state = KY_MOD_STATE_FAILED; break;
        case ModState::Faulted: out->state = KY_MOD_STATE_FAULTED; break;
        case ModState::SkippedSide: out->state = KY_MOD_STATE_SKIPPED; break;
        case ModState::New: out->state = KY_MOD_STATE_NEW; break;
        case ModState::Missing: out->state = KY_MOD_STATE_MISSING; break;
    }
    out->dependency_count = 0;
    out->dependent_count = 0;
    const bool deps_fit = out->size >= sizeof(KyubuModInfo);   // the arrays only when the caller's size covers them
    if (deps_fit && m->manifest_ok) {
        for (auto& d : m->manifest.dependencies) {
            if (out->dependency_count >= KY_MOD_DEPS_MAX) break;
            fill_dep(d, &out->dependencies[out->dependency_count++]);
        }
        ModManager& mm = ModManager::get();
        for (auto& other : mm.all()) {
            if (other.get() == m || !other->manifest_ok) continue;
            for (auto& d : other->manifest.dependencies) {
                if (d.id != m->id) continue;
                if (out->dependent_count >= KY_MOD_DEPS_MAX) break;
                fill_dep(d, &out->dependents[out->dependent_count++], other->id);
            }
        }
    }
}

}  // namespace

KyubuResult manage_mods_get(uint32_t index, KyubuModInfo* out) {
    if (!out) return KY_E_INVALID_ARG;
    if (out->size < (uint32_t)offsetof(KyubuModInfo, dependencies)) return KY_E_INVALID_ARG;
    ModManager& mm = ModManager::get();
    const auto& mods = mm.all();
    Mod* m = nullptr;
    if (index < mods.size()) {
        m = mods[index].get();
    } else {
        std::lock_guard<std::mutex> g(g_rescan_mu);
        const size_t at = index - mods.size();
        if (at >= g_rescan_added.size()) return KY_E_NOT_FOUND;
        m = g_rescan_added[at].get();
    }
    open_pkg(m);                        // discovery opens nothing; the listing needs zip state and size
    if (m->pkg) {
        out->is_zip = m->pkg->is_zip() ? 1u : 0u;
        out->package_size = size_of(*m->pkg);
    }
    fill_info(m, out);
    return KY_OK;
}

// ----------------------------------------------------------------- the package hash
namespace {

struct HashEntry {
    uint64_t size = 0, stamp = 0;
    uint8_t digest[32];
};

std::mutex g_hash_mu;
std::map<std::string, HashEntry> g_hash_cache;

// The canonical listing of a folder package (D5): "<path> <size> <sha256>" per file, sorted by path.
std::string folder_listing(Package& pkg) {
    auto files = pkg.list("");
    std::sort(files.begin(), files.end());
    std::string text;
    for (auto& rel : files) {
        auto bytes = pkg.read(rel);
        if (!bytes) continue;                       // a vanished file is a rehash away
        uint8_t one[32];
        Sha256 s;
        s.update(bytes->data(), bytes->size());
        s.final(one);
        text += rel + " " + std::to_string(bytes->size()) + " " + Sha256::to_hex(one) + "\n";
    }
    return text;
}

}  // namespace

KyubuResult manage_package_hash(const char* id, uint8_t out[32]) {
    if (!id || !*id || !out) return KY_E_INVALID_ARG;
    Mod* found = find_package(id);                          // a disabled or failed package still has one
    if (!found) return KY_E_NOT_FOUND;

    // discovery opens nothing; hashing a zip needs nothing more, a folder needs its file list
    if (!open_pkg(found)) return KY_E_INTERNAL;
    Package& pkg = *found->pkg;

    // cached by path, size and mtime (design.md "Package hash"). A folder's stamp is the folder's own mtime,
    // which Windows does not bump when a file inside changes: such a change rehashes after the next boot or a
    // mods_rescan that reopens the package, not at once. Good enough for a mark that ModManager asks once.
    const uint64_t size = size_of(pkg);
    const uint64_t stamp = stamp_of(pkg.path());
    {
        std::lock_guard<std::mutex> g(g_hash_mu);
        auto it = g_hash_cache.find(pkg.path());
        if (it != g_hash_cache.end() && it->second.size == size && it->second.stamp == stamp) {
            std::memcpy(out, it->second.digest, 32);
            return KY_OK;
        }
    }

    std::string text;
    if (pkg.is_zip()) {
        auto bytes = read_file(pkg.path());
        if (!bytes) return KY_E_INTERNAL;
        text.assign(bytes->begin(), bytes->end());
    } else {
        text = folder_listing(pkg);
    }

    uint8_t digest[32];
    Sha256 s;
    s.update(text.data(), text.size());
    s.final(digest);

    {
        std::lock_guard<std::mutex> g(g_hash_mu);
        HashEntry& e = g_hash_cache[pkg.path()];
        e.size = size;
        e.stamp = stamp;
        std::memcpy(e.digest, digest, 32);
    }
    std::memcpy(out, digest, 32);
    return KY_OK;
}

// ----------------------------------------------------------------- the options of any mod
namespace {

// Reads `id`'s declared options into `out`, whose path points at the player's real file for mods_option_set.
// DeclOk when the declaration parsed (the values are the defaults until load()), DeclNone when the package has
// no config.toml, DeclBad when it does not parse (one warning per id).
DeclStatus read_options(Mod* m, ModConfig* out) {
    if (!open_pkg(m)) return DeclStatus::Bad;
    if (!m->pkg->exists("config.toml")) return DeclStatus::None;
    auto text = m->pkg->read("config.toml");
    std::string src = "mods/" + file_name(m->path) + "/config.toml";
    std::vector<std::string> errors;
    auto decls = parse_config_decl(text ? std::string(text->begin(), text->end()) : std::string(), src, errors);
    if (!decls) {
        log_decl_error_once(m->id, joined(errors));
        return DeclStatus::Bad;
    }
    out->mod_id = m->id;
    out->mod_name = m->manifest_ok && !m->manifest.name.empty() ? m->manifest.name : m->id;
    out->path = join_path(join_path(L().kyubu_dir, "config"), m->id + ".toml");
    out->display = "kyubu/config/" + m->id + ".toml";
    out->decls = std::move(*decls);
    return DeclStatus::Ok;
}

// One entry of the listing: the declaration and the value in use, as TOML texts.
void fill_option(const OptionDecl& d, const std::string& value, KyubuOptionInfo* out) {
    put(out->key, sizeof out->key, d.key);
    put(out->type, sizeof out->type, opt_type_name(d.type));
    put(out->label, sizeof out->label, d.label);
    put_clipped(out->group, sizeof out->group, d.group);
    put_clipped(out->description, sizeof out->description, d.description);
    put_clipped(out->def, sizeof out->def, format_value(d.def));
    put_clipped(out->value, sizeof out->value, value);
    put_clipped(out->min, sizeof out->min, d.min ? format_value(*d.min) : std::string());
    put_clipped(out->max, sizeof out->max, d.max ? format_value(*d.max) : std::string());
    std::string choices;
    for (auto& c : d.choices) choices += (choices.empty() ? "" : ", ") + c;
    put_clipped(out->choices, sizeof out->choices, choices);
    out->restart = d.restart ? 1u : 0u;
}

// The value in use of `d`: the db values the config holds (loaded mod) or what the player's file gave.
std::string value_of(ModConfig& c, const OptionDecl& d) {
    auto it = c.values.find(d.key);
    return it == c.values.end() ? format_value(d.def) : format_value(it->second);
}

}  // namespace

uint32_t manage_option_count(const char* id) {
    if (!id || !*id) return 0;
    Mod* m = find_package(id);
    if (!m) return 0;
    if (m->config) {
        std::lock_guard<std::recursive_mutex> g(m->config->mu);
        return (uint32_t)m->config->decls.size();
    }
    ModConfig view;
    if (read_options(m, &view) != DeclStatus::Ok) return 0;
    return (uint32_t)view.decls.size();
}

KyubuResult manage_option_get(const char* id, uint32_t index, KyubuOptionInfo* out) {
    if (!id || !*id || !out) return KY_E_INVALID_ARG;
    Mod* m = find_package(id);
    if (!m) return KY_E_NOT_FOUND;
    if (m->config) {
        ModConfig& c = *m->config;
        std::lock_guard<std::recursive_mutex> g(c.mu);
        if (index >= c.decls.size()) return KY_E_NOT_FOUND;
        fill_option(c.decls[index], value_of(c, c.decls[index]), out);
        return KY_OK;
    }
    ModConfig view;
    const DeclStatus st = read_options(m, &view);
    if (st != DeclStatus::Ok) return st == DeclStatus::None ? KY_E_NOT_FOUND : KY_E_INTERNAL;
    std::vector<std::string> warnings;
    view.load(false, warnings);         // the player's file or the defaults; nothing is written
    if (index >= view.decls.size()) return KY_E_NOT_FOUND;
    fill_option(view.decls[index], value_of(view, view.decls[index]), out);
    return KY_OK;
}

// The declaration of `key` for the option members, and how the package's config.toml turned out: a live
// config is used as it is, everything else is parsed on request. KY_E_NOT_FOUND (no such key or no options
// at all) and KY_E_INTERNAL (it does not parse) say the same to every caller.
KyubuResult find_option_decl(Mod* m, const std::string& key, ModConfig& view, ModConfig** out) {
    if (m->config) {
        *out = m->config.get();
    } else {
        const DeclStatus st = read_options(m, &view);
        if (st != DeclStatus::Ok) return st == DeclStatus::None ? KY_E_NOT_FOUND : KY_E_INTERNAL;
        *out = &view;
    }
    return (*out)->decl(key) ? KY_OK : KY_E_NOT_FOUND;
}

KyubuResult manage_option_check(const char* id, const char* key, const char* value) {
    if (!id || !*id || !key || !*key || !value) return KY_E_INVALID_ARG;
    Mod* m = find_package(id);
    if (!m) return KY_E_NOT_FOUND;
    ModConfig view;
    ModConfig* c = nullptr;
    const KyubuResult r = find_option_decl(m, key, view, &c);
    if (r != KY_OK) return r;
    const OptionDecl* d = c->decl(key);
    OptValue v;
    if (!parse_value_text(*d, value, &v) || !check_value(*d, v, nullptr)) {
        Log::get().write(KY_LOG_WARN, m->id, KY_SIDE_NONE,
                         "mods_option_check: " + std::string(key) + " = " + value + " is not allowed (" +
                             describe_allowed(*d) + ")");
        return KY_E_INVALID_ARG;
    }
    return KY_OK;
}

// The one place a package's options file is written when the mod is not loaded (D14, and the queue's config
// op): re-parse the declaration, create the file with every default when it does not exist, rewrite one key.
// KY_E_NOT_FOUND for an unknown key, KY_E_INTERNAL when the declaration does not parse, KY_E_INVALID_ARG for a
// value the declaration refuses.
KyubuResult write_option_file(Mod* m, const char* key, const char* value) {
    ModConfig view;
    ModConfig* c = nullptr;
    const KyubuResult known = find_option_decl(m, key, view, &c);
    if (known != KY_OK) return known;
    const OptionDecl* d = c->decl(key);
    std::vector<std::string> warnings;
    view.load(true, warnings);          // D14: a missing file is created with every default
    OptValue v;
    if (!parse_value_text(*d, value, &v)) {
        Log::get().write(KY_LOG_WARN, m->id, KY_SIDE_NONE,
                         "mods_option_set: " + std::string(key) + " = " + value + " is not allowed (" +
                             describe_allowed(*d) + ")");
        return KY_E_INVALID_ARG;
    }
    std::string why;
    KyubuResult r = view.set(key, v, &why, nullptr);
    if (r != KY_OK) {
        Log::get().write(KY_LOG_WARN, m->id, KY_SIDE_NONE, "mods_option_set: " + why);
        return r;
    }
    Log::get().write(KY_LOG_INFO, m->id, KY_SIDE_NONE,
                     std::string("options: ") + key + " = " + format_value(v) + " (saved to " + view.display + ")");
    return KY_OK;
}

KyubuResult manage_option_set(const char* id, const char* key, const char* value) {
    if (!id || !*id || !key || !*key || !value) return KY_E_INVALID_ARG;
    Mod* m = find_package(id);
    if (!m) return KY_E_NOT_FOUND;

    if (m->config) {                    // D14: a loaded mod: exactly its own config_set path
        std::string why;
        KyubuResult r = config_set_text(m, key, value, &why);
        if (r != KY_OK)
            Log::get().write(KY_LOG_WARN, m->id, current_side_tls(), "mods_option_set: " + why);
        return r;
    }

    return write_option_file(m, key, value);   // D14: the file, created with the defaults when missing
}

// ----------------------------------------------------------------- the queue (D2)
namespace {

// One queued change as the loader keeps it; the C struct (KyubuModOp) is the API's copy.
struct QueueOp {
    int kind = KY_MOD_OP_INSTALL;
    std::string id, version, from, key, value;
    bool keep_settings = false;
};

std::mutex g_queue_mu;
std::vector<QueueOp> g_queue;

const char* op_kind_name(int kind) {
    switch (kind) {
        case KY_MOD_OP_INSTALL: return "install";
        case KY_MOD_OP_ENABLE: return "enable";
        case KY_MOD_OP_DISABLE: return "disable";
        case KY_MOD_OP_DELETE: return "delete";
        case KY_MOD_OP_CONFIG: return "config";
    }
    return "?";
}

std::string pending_path() { return join_path(L().kyubu_dir, "pending.toml"); }
std::string backup_dir() { return join_path(L().kyubu_dir, "backup"); }
std::string staging_dir() { return join_path(L().kyubu_dir, "staging"); }
std::string last_apply_path() { return join_path(backup_dir(), "last-apply.toml"); }

// A TOML string literal, escaped by the formatter the loader already carries.
std::string toml_str(const std::string& s) {
    std::ostringstream os;
    os << toml::value<std::string>(s);
    return os.str();
}

// The pending file (D2): one inline table per change, in queue order, readable and editable by a player.
std::string pending_text(const std::vector<QueueOp>& q) {
    std::string text = "# kyubu\\pending.toml, written by mod_manager on " + now_stamp() + "\n";
    if (q.empty()) return text + "# no changes queued\n";
    text += "op = [\n";
    for (auto& op : q) {
        text += "  { kind = " + toml_str(op_kind_name(op.kind)) + ", id = " + toml_str(op.id);
        if (op.kind == KY_MOD_OP_INSTALL)
            text += ", version = " + toml_str(op.version) + ", from = " + toml_str(op.from);
        if (op.kind == KY_MOD_OP_CONFIG)
            text += ", key = " + toml_str(op.key) + ", value = " + toml_str(op.value);
        if (op.kind == KY_MOD_OP_DELETE)
            text += std::string(", keep_settings = ") + (op.keep_settings ? "true" : "false");
        text += " },\n";
    }
    return text + "]\n";
}

bool write_pending(const std::vector<QueueOp>& q) { return write_text(pending_path(), pending_text(q)); }

// A `from` is a game-relative path with '/' inside kyubu\staging (an install) or kyubu\backup (an undo).
// Nothing else: no drive, no '..', no absolute path. The pending file is plain text a player may edit.
std::string normalized_from(const std::string& from) {
    std::string f = from;
    for (char& c : f) if (c == '\\') c = '/';
    return f;
}

bool valid_from(const std::string& from) {
    const std::string f = normalized_from(from);
    if (f.empty() || f[0] == '/' || f.find("..") != std::string::npos || f.find(':') != std::string::npos) return false;
    return starts_with(f, "kyubu/staging/") || starts_with(f, "kyubu/backup/");
}

// Resolves a `from` against kyubu_dir (which is <game>\kyubu), so the tests need no game folder.
std::string from_path(const std::string& from) {
    std::string f = normalized_from(from);
    if (starts_with(f, "kyubu/")) f = f.substr(6);
    return join_path(L().kyubu_dir, f);
}

// "kyubu/staging/x.zip" as it sits under kyubu_dir ("staging/x.zip"), for the staging clean-up set.
std::string under_kyubu(const std::string& game_relative) {
    std::string f = normalized_from(game_relative);
    return starts_with(f, "kyubu/") ? f.substr(6) : f;
}

// A package on disk, as boot-time discovery would read it.
struct DiskPackage {
    std::string path;
    bool zip = false;
    bool manifest_ok = false;
    Manifest manifest;
};

bool read_package(const std::string& path, DiskPackage* out) {
    auto pkg = Package::open(path);
    if (!pkg || !pkg->exists("mod.toml")) return false;
    auto text = pkg->read("mod.toml");
    if (!text) return false;
    std::vector<std::string> errors;
    auto man = Manifest::parse(std::string(text->begin(), text->end()), "mods/" + file_name(path) + "/mod.toml", errors);
    out->path = path;
    out->zip = pkg->is_zip();
    if (man) {
        out->manifest = *man;
        out->manifest_ok = true;
    }
    return true;
}

// The installed package of `id` (a folder or a zip with a mod.toml), as the queue's ops find it before the
// boot scan: the listing in ModManager is empty at that point.
bool find_installed(const std::string& id, DiskPackage* out) {
    for (const std::string& name : list_dir(L().mods_dir)) {
        const std::string p = join_path(L().mods_dir, name);
        if (!dir_exists(p) && !ends_with(lower(name), ".zip")) continue;
        DiskPackage d;
        if (!read_package(p, &d)) continue;
        if (d.manifest_ok && d.manifest.id == id) {
            *out = d;
            return true;
        }
    }
    return false;
}

// A package as a stand-in Mod, for the option helpers (which read m->pkg, m->id and the manifest name).
bool open_as_mod(const std::string& path, Mod& m) {
    DiskPackage d;
    if (!read_package(path, &d)) return false;
    m.path = path;
    m.pkg = Package::open(path);
    m.manifest = d.manifest;
    m.manifest_ok = d.manifest_ok;
    m.id = d.manifest_ok ? d.manifest.id : file_name(path);
    return true;
}

// Where the declaration of a config op comes from: a staged install of the same id in the queue, else the installed
// package (a modpack sets the options of a mod it also installs; D2).
bool config_package_path(const std::string& id, const std::vector<QueueOp>& q, std::string& out) {
    // A queued install comes first: boot writes options after the installs, so its declaration is the one that counts
    // (a pack that pins another version).
    for (auto it = q.rbegin(); it != q.rend(); ++it) {
        if (it->kind != KY_MOD_OP_INSTALL || it->id != id) continue;
        const std::string p = from_path(it->from);
        if (file_exists(p) || dir_exists(p)) {
            out = p;
            return true;
        }
    }
    DiskPackage d;
    if (find_installed(id, &d)) {
        out = d.path;
        return true;
    }
    return false;
}

// Shape and value checks of one op, before it reaches the queue: a refused op leaves the pending file alone.
KyubuResult queue_check(const QueueOp& op, const std::vector<QueueOp>& with_new) {
    if (op.kind < KY_MOD_OP_INSTALL || op.kind > KY_MOD_OP_CONFIG) return KY_E_INVALID_ARG;
    if (!valid_mod_id(op.id)) return KY_E_INVALID_ARG;
    switch (op.kind) {
        case KY_MOD_OP_INSTALL:
            if (!Version::parse(op.version) || !valid_from(op.from)) return KY_E_INVALID_ARG;
            return KY_OK;
        case KY_MOD_OP_ENABLE:
        case KY_MOD_OP_DISABLE:
        case KY_MOD_OP_DELETE:
            return KY_OK;
        case KY_MOD_OP_CONFIG: {
            if (op.key.empty() || op.value.empty()) return KY_E_INVALID_ARG;
            std::string path;
            if (!config_package_path(op.id, with_new, path)) return KY_E_NOT_FOUND;
            Mod m;
            if (!open_as_mod(path, m)) return KY_E_INTERNAL;
            ModConfig view;
            ModConfig* c = nullptr;
            const KyubuResult known = find_option_decl(&m, op.key, view, &c);
            if (known != KY_OK) return known;
            const OptionDecl* d = c->decl(op.key);
            OptValue v;
            if (!parse_value_text(*d, op.value, &v) || !check_value(*d, v, nullptr)) return KY_E_INVALID_ARG;
            return KY_OK;
        }
    }
    return KY_E_INVALID_ARG;
}

// One C field, bounded: the caller's char array need not be terminated.
std::string field(const char* s, size_t cap) {
    size_t n = 0;
    while (n < cap && s[n]) ++n;
    return std::string(s, n);
}

void fill_op(const QueueOp& op, KyubuModOp* out) {
    std::memset(out, 0, sizeof *out);
    out->kind = (uint32_t)op.kind;
    put(out->id, sizeof out->id, op.id);
    put(out->version, sizeof out->version, op.version);
    put(out->from, sizeof out->from, op.from);
    put(out->key, sizeof out->key, op.key);
    put(out->value, sizeof out->value, op.value);
    out->keep_settings = op.keep_settings ? 1u : 0u;
}

}  // namespace

KyubuResult manage_queue(const KyubuModOp* op) {
    if (!op) return KY_E_INVALID_ARG;
    QueueOp q;
    q.kind = (int)op->kind;
    q.id = field(op->id, sizeof op->id);
    q.version = field(op->version, sizeof op->version);
    q.from = field(op->from, sizeof op->from);
    q.key = field(op->key, sizeof op->key);
    q.value = field(op->value, sizeof op->value);
    q.keep_settings = op->keep_settings != 0;

    std::lock_guard<std::mutex> g(g_queue_mu);
    if (g_queue.size() >= 64) return KY_E_TOO_LARGE;
    std::vector<QueueOp> after = g_queue;
    after.push_back(q);
    const KyubuResult r = queue_check(q, after);
    if (r != KY_OK) {
        KWARN("mods_queue: %s %s refuses: queued nothing", op_kind_name(q.kind), q.id.c_str());
        return r;
    }
    if (!write_pending(after)) return KY_E_INTERNAL;
    g_queue = std::move(after);
    return KY_OK;
}

uint32_t manage_queue_count() {
    std::lock_guard<std::mutex> g(g_queue_mu);
    return (uint32_t)g_queue.size();
}

KyubuResult manage_queue_get(uint32_t index, KyubuModOp* out) {
    if (!out) return KY_E_INVALID_ARG;
    std::lock_guard<std::mutex> g(g_queue_mu);
    if (index >= g_queue.size()) return KY_E_NOT_FOUND;
    fill_op(g_queue[index], out);
    return KY_OK;
}

KyubuResult manage_queue_clear() {
    std::lock_guard<std::mutex> g(g_queue_mu);
    g_queue.clear();
    std::error_code ec;
    fs::remove(fs::path(widen(pending_path())), ec);
    return KY_OK;
}


// ----------------------------------------------------------------- the rescan (D15)
KyubuResult manage_rescan() {
    Loader& l = L();
    ModManager& mm = ModManager::get();

    // the paths on disk now, under the discover rules: a folder, or a zip
    std::vector<std::string> present;   // original case, for Package::open and the listing
    for (auto& name : list_dir(l.mods_dir)) {
        const std::string p = join_path(l.mods_dir, name);
        if (!dir_exists(p) && !ends_with(lower(name), ".zip")) continue;   // a stray file is not a package
        present.push_back(p);
    }

    // the paths the boot scan already owns (case-insensitively: Windows does not care about case)
    std::set<std::string> taken;
    for (auto& m : mm.all()) taken.insert(lower(m->path));

    // the paths present now, for the missing check
    std::set<std::string> on_disk;
    for (const std::string& p : present) on_disk.insert(lower(p));

    // read the manifests of the packages added since boot, as discover does
    std::vector<std::unique_ptr<Mod>> added;
    for (const std::string& p : present) {        if (taken.count(lower(p))) continue;
        auto pkg = Package::open(p);
        if (!pkg || !pkg->exists("mod.toml")) continue;   // no mod.toml: not a package, as at boot
        added.emplace_back(std::make_unique<Mod>());
        Mod& m = *added.back();
        m.path = p;
        m.pkg = std::move(pkg);
        auto text = m.pkg->read("mod.toml");
        std::vector<std::string> errors;
        auto man = Manifest::parse(std::string(text->begin(), text->end()),
                                   "mods/" + file_name(p) + "/mod.toml", errors);
        for (int s = 1; s <= 2; ++s) {
            m.ctx[s].mod = &m;
            m.ctx[s].side = (KyubuSide)s;
        }
        if (man) {                          // the state `new`: nothing is loaded, it needs a restart (D15)
            m.manifest = *man;
            m.id = m.manifest.id;
            m.state = ModState::New;
        } else {                            // it does not parse: state `failed` with the error (D15)
            m.manifest_ok = false;
            m.id = file_name(p);
            m.state = ModState::Failed;
            m.reason = joined(errors);
        }
    }

    // the discovered packages whose path is gone are `missing`; one that came back loses only the mark
    for (auto& m : mm.all()) {
        if (on_disk.count(lower(m->path))) {
            if (m->state == ModState::Missing)
                m->state = l.config.is_enabled(m->id) ? ModState::Loaded : ModState::Disabled;
        } else {
            m->state = ModState::Missing;
        }
    }

    std::lock_guard<std::mutex> g(g_rescan_mu);
    g_rescan_added = std::move(added);
    return KY_OK;
}

// ----------------------------------------------------------------- applying the queue at boot (D2)
namespace {

// One package the apply moved into kyubu\backup\, for last-apply.toml.
struct MovedPackage {
    std::string id, from, to;
};

// An absolute path in the game folder as the two files write it: kyubu\... as "kyubu/...", mods\... as
// "mods/...". Anything else stays absolute.
std::string game_relative(const std::string& path) {
    const struct {
        const std::string* root;
        const char* name;
    } roots[] = {{&L().kyubu_dir, "kyubu/"}, {&L().mods_dir, "mods/"}};
    for (auto& r : roots) {
        const std::string& k = *r.root;
        if (k.empty() || path.size() <= k.size() + 1 || lower(path.substr(0, k.size())) != lower(k)) continue;
        std::string rel = path.substr(k.size());
        while (!rel.empty() && (rel.front() == '\\' || rel.front() == '/')) rel.erase(rel.begin());
        for (char& c : rel) if (c == '\\') c = '/';
        return std::string(r.name) + rel;
    }
    return path;
}

bool move_path(const std::string& from, const std::string& to) {
    std::error_code ec;
    fs::rename(fs::path(widen(from)), fs::path(widen(to)), ec);
    if (!ec) return true;
    if (!dir_exists(from)) return false;            // a zip is a file; a folder is copied file by file
    fs::copy(fs::path(widen(from)), fs::path(widen(to)), fs::copy_options::recursive, ec);
    if (ec) return false;
    fs::remove_all(fs::path(widen(from)), ec);
    return !ec;
}

// A free name in kyubu\backup\: "<id>-<version>", then "-2", "-3" for a second copy of the same one.
std::string backup_target(const std::string& base, bool zip) {
    make_dirs(backup_dir());
    const std::string ext = zip ? ".zip" : "";
    std::string name = base + ext;
    for (int n = 2; file_exists(join_path(backup_dir(), name)) || dir_exists(join_path(backup_dir(), name)); ++n)
        name = base + "-" + std::to_string(n) + ext;
    return join_path(backup_dir(), name);
}

bool backup_move(const DiskPackage& d, std::vector<MovedPackage>& moved, std::string* why) {
    const std::string base = d.manifest_ok ? d.manifest.id + "-" + d.manifest.version.str() : file_name(d.path);
    const std::string to = backup_target(base, d.zip);
    if (!move_path(d.path, to)) {
        *why = "cannot move " + file_name(d.path) + " into kyubu\\backup\\";
        return false;
    }
    moved.push_back({d.manifest_ok ? d.manifest.id : base, d.path, to});
    KINFO("pending.toml: %s moved to %s", file_name(d.path).c_str(), game_relative(to).c_str());
    return true;
}

// The replaced package's kyubu\cache\<id>-* folders go (D2); a cache entry is only an extraction of the old zip.
void remove_cache(const std::string& id) {
    const std::string dir = join_path(L().kyubu_dir, "cache");
    const std::string prefix = lower(id) + "-";
    for (const std::string& name : list_dir(dir)) {
        if (!starts_with(lower(name), prefix)) continue;
        std::error_code ec;
        fs::remove_all(fs::path(widen(join_path(dir, name))), ec);
    }
}

bool run_install(const QueueOp& op, std::vector<MovedPackage>& moved, std::string* why) {
    const std::string from = from_path(op.from);
    if (!file_exists(from) && !dir_exists(from)) {
        *why = "the staged package is missing";
        return false;
    }
    DiskPackage staged;
    if (!read_package(from, &staged)) {
        *why = "the staged package has no mod.toml";
        return false;
    }
    if (!staged.manifest_ok) {
        *why = "the staged mod.toml does not parse";
        return false;
    }
    if (staged.manifest.id != op.id) {
        *why = "the staged package is '" + staged.manifest.id + "', not '" + op.id + "'";
        return false;
    }
    if (staged.manifest.version.str() != op.version) {
        *why = "the staged package is version " + staged.manifest.version.str() + ", not " + op.version;
        return false;
    }
    // every package of the same id already there goes to the backup, never deleted (D2)
    DiskPackage old;
    while (find_installed(op.id, &old))
        if (!backup_move(old, moved, why)) return false;
    const std::string dest = join_path(L().mods_dir, op.id + "-" + op.version + (staged.zip ? ".zip" : ""));
    if (file_exists(dest) || dir_exists(dest)) {    // a package whose manifest does not parse has no id to match
        DiskPackage stale;
        if (!read_package(dest, &stale)) {
            stale.path = dest;
            stale.zip = staged.zip;
        }
        if (!backup_move(stale, moved, why)) return false;
    }
    if (!move_path(from, dest)) {
        *why = "cannot move the staged package into mods\\";
        return false;
    }
    remove_cache(op.id);
    KINFO("pending.toml: installed %s %s", op.id.c_str(), op.version.c_str());
    return true;
}

bool run_delete(const QueueOp& op, std::vector<MovedPackage>& moved, std::string* why) {
    DiskPackage d;
    if (!find_installed(op.id, &d)) {
        *why = "it is not installed";
        return false;
    }
    if (!backup_move(d, moved, why)) return false;
    if (!op.keep_settings) {
        std::error_code ec;
        fs::remove(fs::path(widen(join_path(join_path(L().kyubu_dir, "config"), op.id + ".toml"))), ec);
        fs::remove_all(fs::path(widen(join_path(join_path(L().kyubu_dir, "data"), op.id))), ec);
    }
    KINFO("pending.toml: deleted %s (settings %s)", op.id.c_str(), op.keep_settings ? "kept" : "removed");
    return true;
}

bool run_config(const QueueOp& op, std::string* why) {
    DiskPackage d;
    if (!find_installed(op.id, &d)) {
        *why = "it is not installed";
        return false;
    }
    Mod m;
    if (!open_as_mod(d.path, m)) {
        *why = "cannot open the package";
        return false;
    }
    const KyubuResult r = write_option_file(&m, op.key.c_str(), op.value.c_str());
    if (r == KY_E_NOT_FOUND) {
        *why = "no option '" + op.key + "'";
        return false;
    }
    if (r != KY_OK) {
        *why = "the value is not allowed";
        return false;
    }
    return true;
}

bool run_op(const QueueOp& op, std::vector<MovedPackage>& moved, std::string* why) {
    switch (op.kind) {
        case KY_MOD_OP_INSTALL: return run_install(op, moved, why);
        case KY_MOD_OP_ENABLE:
        case KY_MOD_OP_DISABLE: {
            const bool on = op.kind == KY_MOD_OP_ENABLE;
            if (!Config::set_mod_enabled(L().config_path, op.id, on)) {
                *why = "cannot write kyubu\\config.toml";
                return false;
            }
            L().config.mod_enabled[op.id] = on;     // the scan after this reads it
            KINFO("pending.toml: %s %s", op.id.c_str(), on ? "enabled" : "disabled");
            return true;
        }
        case KY_MOD_OP_DELETE: return run_delete(op, moved, why);
        case KY_MOD_OP_CONFIG: return run_config(op, why);
    }
    *why = "unknown kind";
    return false;
}

// kyubu\backup\last-apply.toml (D2): the previous [mods] of config.toml, the previous options files, and the
// packages that went to the backup. `config_before` is the text read before the first op (a rewrite at the end
// must not read the state the ops left); the packages join it once they moved, so a failed move is not offered
// for undo.
std::string last_apply_text(const std::vector<QueueOp>& ops, const std::string& config_before,
                            const std::map<std::string, std::string>& options,
                            const std::vector<MovedPackage>& moved) {
    toml::table t;
    if (!config_before.empty()) {
        auto res = toml::parse(config_before, L().config_path);
        if (res)
            if (auto mods = res.table()["mods"].as_table()) t.insert_or_assign("mods", toml::table(*mods));
    }
    toml::table opt;
    for (auto& [id, text] : options) opt.insert_or_assign(id, text);
    t.insert_or_assign("options", std::move(opt));
    if (!moved.empty()) {
        toml::array arr;
        for (auto& m : moved) {
            toml::table e;
            e.insert("id", m.id);
            e.insert("from", game_relative(m.from));
            e.insert("to", game_relative(m.to));
            arr.push_back(std::move(e));
        }
        t.insert_or_assign("package", std::move(arr));
    }
    std::ostringstream os;
    os << "# kyubu\\backup\\last-apply.toml, written by the loader before applying " << ops.size()
       << " queued change(s) on " << now_stamp() << "\n"
       << t;
    return os.str();
}

void write_last_apply(const std::vector<QueueOp>& ops, const std::string& config_before,
                      const std::map<std::string, std::string>& options, const std::vector<MovedPackage>& moved) {
    if (!write_text(last_apply_path(), last_apply_text(ops, config_before, options, moved)))
        KWARN("pending.toml: cannot write kyubu\\backup\\last-apply.toml");
}

// A boot with no pending file ends the one undo window (D2): the packages in kyubu\backup\ go. Other files
// there stay: boot.cpp keeps a copy of zlib1.dll in it.
void purge_backup() {
    bool any = false;
    for (const std::string& name : list_dir(backup_dir())) {
        if (lower(name) == "last-apply.toml") continue;
        const std::string p = join_path(backup_dir(), name);
        if (!dir_exists(p) && !ends_with(lower(name), ".zip")) continue;
        std::error_code ec;
        fs::remove_all(fs::path(widen(p)), ec);
        if (!ec) {
            any = true;
            KINFO("pending.toml: backup %s purged", name.c_str());
        }
    }
    std::error_code ec;
    fs::remove(fs::path(widen(last_apply_path())), ec);
    if (any) KINFO("pending.toml: no pending file, the packages of the last apply are gone");
}

// Staged files that no pending op names are deleted at boot (D2); a named one is the op's own business.
void clean_staging(const std::vector<QueueOp>& ops) {
    std::set<std::string> named;
    for (auto& op : ops)
        if (op.kind == KY_MOD_OP_INSTALL) named.insert(lower(under_kyubu(op.from)));
    for (const std::string& name : list_dir(staging_dir())) {
        if (named.count(lower("staging/" + name))) continue;
        std::error_code ec;
        fs::remove_all(fs::path(widen(join_path(staging_dir(), name))), ec);
        if (!ec) KINFO("pending.toml: staged %s deleted (no op names it)", name.c_str());
    }
}

// The pending file into ops. False when the file itself does not parse: nobody can then say how many ops it had.
bool read_pending(std::vector<QueueOp>& ops, std::string* why) {
    auto data = read_file(pending_path());
    if (!data) {
        *why = "cannot read it";
        return false;
    }
    auto res = toml::parse(std::string(data->begin(), data->end()), pending_path());
    if (!res) {
        *why = fmt_toml_error(res.error());
        return false;
    }
    auto arr = res.table()["op"].as_array();
    if (!arr) return true;                          // an empty queue
    for (auto& e : *arr) {
        QueueOp q;
        q.kind = -1;
        if (auto t = e.as_table()) {
            const std::string kind = (*t)["kind"].value_or(std::string());
            if (kind == "install") q.kind = KY_MOD_OP_INSTALL;
            else if (kind == "enable") q.kind = KY_MOD_OP_ENABLE;
            else if (kind == "disable") q.kind = KY_MOD_OP_DISABLE;
            else if (kind == "delete") q.kind = KY_MOD_OP_DELETE;
            else if (kind == "config") q.kind = KY_MOD_OP_CONFIG;
            q.id = (*t)["id"].value_or(std::string());
            q.version = (*t)["version"].value_or(std::string());
            q.from = (*t)["from"].value_or(std::string());
            q.key = (*t)["key"].value_or(std::string());
            q.value = (*t)["value"].value_or(std::string());
            q.keep_settings = (*t)["keep_settings"].value_or(false);
        }
        ops.push_back(std::move(q));
    }
    return true;
}

}  // namespace

PendingResult apply_pending() {
    PendingResult r;
    if (!file_exists(pending_path())) {
        purge_backup();
        return r;
    }
    if (L().safe_mode) {
        r.skipped = true;
        KINFO("pending.toml: safe mode (%s), the queue is kept for the next normal start", L().safe_reason.c_str());
        return r;
    }
    std::vector<QueueOp> ops;
    std::string why;
    if (!read_pending(ops, &why)) {
        KERROR("pending.toml: %s; the file is deleted, nothing is applied", why.c_str());
        r.failed = 1;
        std::error_code ec;
        fs::remove(fs::path(widen(pending_path())), ec);
        return r;
    }

    // The previous options files and [mods] of config.toml, read before anything is written (the undo material).
    std::map<std::string, std::string> options;
    for (auto& op : ops) {
        if (op.kind != KY_MOD_OP_CONFIG || options.count(op.id)) continue;
        auto data = read_file(join_path(join_path(L().kyubu_dir, "config"), op.id + ".toml"));
        options[op.id] = data ? std::string(data->begin(), data->end()) : std::string();
    }
    std::string config_before;
    if (auto data = read_file(L().config_path)) config_before.assign(data->begin(), data->end());
    std::vector<MovedPackage> moved;
    write_last_apply(ops, config_before, options, moved);

    // The spec's order: installs, enables and disables, deletes, then options (D2).
    auto pass = [&](int a, int b) {
        for (auto& op : ops) {
            if (op.kind < a || op.kind > b) continue;
            std::string reason;
            if (run_op(op, moved, &reason)) {
                ++r.applied;
            } else {
                ++r.failed;
                KERROR("pending.toml: %s %s failed: %s", op_kind_name(op.kind), op.id.c_str(), reason.c_str());
            }
        }
    };
    pass(KY_MOD_OP_INSTALL, KY_MOD_OP_INSTALL);
    pass(KY_MOD_OP_ENABLE, KY_MOD_OP_DISABLE);
    pass(KY_MOD_OP_DELETE, KY_MOD_OP_DELETE);
    pass(KY_MOD_OP_CONFIG, KY_MOD_OP_CONFIG);
    if (!moved.empty()) write_last_apply(ops, config_before, options, moved);

    std::error_code ec;
    fs::remove(fs::path(widen(pending_path())), ec);
    clean_staging(ops);
    KINFO("pending.toml: %u applied, %u failed; the file is deleted", r.applied, r.failed);
    return r;
}

std::string pending_notice(const PendingResult& r) {
    if (!r.failed) return std::string();
    return "ModManager: " + std::to_string(r.failed) + " of " + std::to_string(r.applied + r.failed) +
           " changes failed, see the log";
}

}  // namespace kyubu
