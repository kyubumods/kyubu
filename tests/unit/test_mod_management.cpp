// The mod-management permission and the listing (add-mod-manager, tasks 3.1 to 3.4): parse manage_mods,
// refuse without it, list every package of mods\, hash them per D5, read and set the options of any mod,
// rescan mods\ after boot, queue changes and apply kyubu\pending.toml at boot.
#include "doctest.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "api.h"
#include "events.h"
#include "game_hooks.h"
#include "host.h"
#include "loader.h"
#include "log.h"
#include "manage.h"
#include "manifest.h"
#include "miniz.h"
#include "modconfig.h"
#include "modmanager.h"
#include "package.h"
#include "sha256.h"
#include "toml_config.h"

using namespace kyubu;
namespace fs = std::filesystem;

namespace kyubu {   // defined in events/game_hooks.cpp for the game's database hooks; the world session starts and ends here
void on_database_opened(void* obj, const std::string& path, bool ok);
void on_sqlite_closed(void* handle);
}  // namespace kyubu

namespace {

// A mod that is not loaded from disk: enough for the permission check (test_mods.cpp has no Mod, test_abilities's DemoMod pulls in the api table).
struct DemoMod {
    Mod mod;
    DemoMod(const char* id, const char* trust, bool manage) {
        mod.id = id;
        mod.state = ModState::Loaded;
        mod.manifest.sides = KY_SIDE_BOTH;
        mod.manifest.trust = trust;
        mod.manifest.manage_mods = manage;
    }
};

bool logged(uint64_t since, const std::string& part) {
    for (auto& e : Log::get().since(since))
        if (e.text.find(part) != std::string::npos) return true;
    return false;
}

const std::string kMfx = KYUBU_SOURCE_DIR "/tests/fixtures/manage";

fs::path P(const std::string& utf8) { return fs::path(widen(utf8)); }

std::string text_of2(const std::string& path) {
    auto d = read_file(path);
    REQUIRE(d.has_value());
    return std::string(d->begin(), d->end());
}

// A temporary game folder whose mods\ holds the packages of fixtures/manage, discovered the loader's own way.
struct ManageGame {
    Loader& l = L();
    ModManager& mm = ModManager::get();
    const std::string saved_kyubu = l.kyubu_dir, saved_mods = l.mods_dir, saved_game = l.game_dir;
    const std::string saved_config = l.config_path;
    const KyubuHost saved_host = l.host.host;
    const KyubuHostMode saved_mode = l.mode;
    const bool saved_safe = l.safe_mode;
    std::map<std::string, bool> saved_enabled = l.config.mod_enabled;
    std::string dir;

    ManageGame(const char* name) {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        fs::path p = fs::path(tmp) / (std::string("kyubu-manage-") + name + "-" + std::to_string(GetCurrentProcessId()));
        std::error_code ec;
        fs::remove_all(p, ec);
        fs::create_directories(p);
        dir = narrow(p.wstring());
        l.game_dir = dir;
        l.kyubu_dir = dir + "/kyubu";
        l.mods_dir = dir + "/mods";
        l.config_path = l.kyubu_dir + "/config.toml";
        l.safe_mode = false;
        manage_queue_clear();               // the queue of an earlier case has no pending file here
        fs::create_directories(P(l.mods_dir));
        l.host.host = KY_HOST_CLIENT;       // the client sides, so no fixture is skipped for its sides
        l.mode = KY_MODE_MENU_OR_SINGLEPLAYER;
        // the fixtures are copied under their package names: the loader names a package that does not parse
        // (mm_failed) after its folder
        for (const auto& [from, to] : std::vector<std::pair<std::string, std::string>>{
                 {"canonical", "mm_loaded"}, {"disabled", "mm_disabled"}, {"failed", "mm_failed"}, {"restricted", "mm_restricted"}}) {
            fs::copy(P(kMfx + "/" + from), P(l.mods_dir + "/" + to), fs::copy_options::recursive);
        }
        // the zipped fixture becomes mods\mm_zipped.zip: a zip's mtimes exist only at creation, so it is made here
        const std::string text = text_of2(kMfx + "/zipped/mod.toml");
        REQUIRE(mz_zip_add_mem_to_archive_file_in_place((l.mods_dir + "/mm_zipped.zip").c_str(), "mod.toml", text.data(), text.size(), nullptr, 0, MZ_DEFAULT_LEVEL));
        l.config.mod_enabled["mm_disabled"] = false;   // the disabled state of mm_disabled
        mm.discover_and_resolve();
    }
    ~ManageGame() {
        l.mods_dir = dir + "/empty";                   // leave nothing discovered behind
        make_dirs(l.mods_dir);
        mm.discover_and_resolve();
        manage_queue_clear();
        l.mods_dir = saved_mods;
        l.kyubu_dir = saved_kyubu;
        l.game_dir = saved_game;
        l.config_path = saved_config;
        l.host.host = saved_host;
        l.mode = saved_mode;
        l.safe_mode = saved_safe;
        l.config.mod_enabled = saved_enabled;
    }
};

// parses a hex digest string into bytes.
bool unhex(const std::string& hex, uint8_t out[32]) {
    if (hex.size() != 64) return false;
    for (int i = 0; i < 32; ++i) {
        auto nib = [&](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return -1;
        };
        const int hi = nib(hex[2 * i]), lo = nib(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

bool same_digest(const uint8_t* a, const uint8_t* b) {
    return std::memcmp(a, b, 32) == 0;
}

// writes text as a UTF-8 (narrow) file, for the config.toml a test adds to a copied fixture
void write_of(const std::string& path, const std::string& text) {
    make_dirs(dir_of(path));
    std::ofstream f(fs::path(widen(path)), std::ios::binary);
    REQUIRE(f.good());
    f << text;
}

// A staged package for the queue tests: a zip at `path` holding mod.toml and, when not empty, config.toml.
void staged_zip(const std::string& path, const std::string& mod_toml, const std::string& config_toml) {
    std::error_code ec;
    fs::remove(P(path), ec);
    make_dirs(dir_of(path));
    REQUIRE(mz_zip_add_mem_to_archive_file_in_place(path.c_str(), "mod.toml", mod_toml.data(), mod_toml.size(),
                                                    nullptr, 0, MZ_DEFAULT_LEVEL));
    if (!config_toml.empty())
        REQUIRE(mz_zip_add_mem_to_archive_file_in_place(path.c_str(), "config.toml", config_toml.data(),
                                                        config_toml.size(), nullptr, 0, MZ_DEFAULT_LEVEL));
}

KyubuModOp make_op(KyubuModOpKind kind, const char* id) {
    KyubuModOp op{};
    op.kind = kind;
    std::snprintf(op.id, sizeof op.id, "%s", id);
    return op;
}

void set_version(KyubuModOp& op, const char* v) { std::snprintf(op.version, sizeof op.version, "%s", v); }
void set_from(KyubuModOp& op, const std::string& v) { std::snprintf(op.from, sizeof op.from, "%s", v.c_str()); }
void set_key_value(KyubuModOp& op, const char* key, const char* value) {
    std::snprintf(op.key, sizeof op.key, "%s", key);
    std::snprintf(op.value, sizeof op.value, "%s", value);
}

}  // namespace

TEST_CASE("mod-management: the management permission") {
    // covers: mod-management/The management permission (allowed, mod without the permission, restricted mod)
    DemoMod allowed("manager", "full", true);
    DemoMod no_flag("noperm", "full", false);
    DemoMod restricted("restricted", "restricted", true);

    const uint64_t seq = Log::get().sequence();
    CHECK(manage_permission(&allowed.mod, KY_SIDE_CLIENT, "mods_queue") == KY_OK);
    CHECK_FALSE(logged(seq, "mods_queue"));

    CHECK(manage_permission(&no_flag.mod, KY_SIDE_CLIENT, "mods_queue") == KY_E_DENIED);
    CHECK(logged(seq, "mods_queue: mod management needs manage_mods = true in mod.toml"));

    CHECK(manage_permission(&restricted.mod, KY_SIDE_CLIENT, "mods_queue") == KY_E_DENIED);
    CHECK(logged(seq, "mods_queue: restricted mods cannot manage mods"));

    // Once per (mod, function): a repeat call logs nothing again.
    const uint64_t seq2 = Log::get().sequence();
    CHECK(manage_permission(&no_flag.mod, KY_SIDE_CLIENT, "mods_queue") == KY_E_DENIED);
    CHECK(manage_permission(&restricted.mod, KY_SIDE_SERVER, "mods_queue") == KY_E_DENIED);
    CHECK_FALSE(logged(seq2, "mods_queue"));
}

TEST_CASE("mod-management: manage_mods is an optional bool of mod.toml") {
    std::vector<std::string> errors;
    auto with = [&](const char* text) {
        errors.clear();
        return Manifest::parse(text, "mod.toml", errors);
    };
    auto m = with(R"(
id = "alpha"
name = "Alpha"
version = "1.2.0"
kyubu = "^1.2"
sides = ["client"]
)");
    REQUIRE(m);
    CHECK(errors.empty());
    CHECK_FALSE(m->manage_mods);   // false by default

    m = with(R"(
id = "alpha"
name = "Alpha"
version = "1.2.0"
kyubu = "^1.2"
sides = ["client"]
manage_mods = true
)");
    REQUIRE(m);
    CHECK(errors.empty());
    CHECK(m->manage_mods);

    m = with(R"(
id = "alpha"
name = "Alpha"
version = "1.2.0"
kyubu = "^1.2"
sides = ["client"]
manage_mods = false
)");
    REQUIRE(m);
    CHECK_FALSE(m->manage_mods);

    m = with(R"(
id = "alpha"
name = "Alpha"
version = "1.2.0"
kyubu = "^1.2"
sides = ["client"]
manage_mods = "yes"
)");
    // No type check, like requires_server: a non-bool is ignored, the default stands.
    REQUIRE(m);
    CHECK_FALSE(m->manage_mods);
}

// ----------------------------------------------------------------- 3.2: the listing and the hash
TEST_CASE("mod-management: mods_count and mods_get list every package") {
    // covers: mod-management/Listing every mod (loaded, disabled, broken manifest, zip, dependents)
    ManageGame g("list");
    const KyubuApi* api = api_table();
    Mod* caller = g.mm.find("mm_loaded");
    REQUIRE(caller);
    REQUIRE(caller->state == ModState::Loaded);

    CHECK(api->mods_count(caller->handle(KY_SIDE_CLIENT)) == 5);

    std::map<std::string, KyubuModInfo> by_id;
    for (uint32_t i = 0; i < 5; ++i) {
        KyubuModInfo info{};
        info.size = sizeof info;
        REQUIRE(api->mods_get(caller->handle(KY_SIDE_CLIENT), i, &info) == KY_OK);
        REQUIRE_FALSE(by_id.count(info.id));
        by_id[info.id] = info;
    }
    REQUIRE(by_id.count("mm_loaded"));
    REQUIRE(by_id.count("mm_disabled"));
    REQUIRE(by_id.count("mm_failed"));
    REQUIRE(by_id.count("mm_restricted"));
    REQUIRE(by_id.count("mm_zipped"));

    const KyubuModInfo& loaded = by_id.at("mm_loaded");
    CHECK(loaded.state == KY_MOD_STATE_LOADED);
    CHECK(std::string(loaded.name) == "Manage: loaded");
    CHECK(std::string(loaded.version) == "1.0.0");
    CHECK(std::string(loaded.kyubu_req) == "^1.1");
    CHECK(std::string(loaded.trust) == "full");
    CHECK(std::string(loaded.authors) == "Kyubu tests");
    CHECK(std::string(loaded.description).find("folder-hash vector") != std::string::npos);
    CHECK(loaded.is_zip == 0);
    CHECK(loaded.package_size > 0);
    CHECK(std::string(loaded.path).find("mm_loaded") != std::string::npos);
    CHECK(loaded.dependency_count == 0);
    CHECK(loaded.dependent_count == 1);
    CHECK(std::string(loaded.dependents[0].id) == "mm_zipped");
    CHECK(std::string(loaded.dependents[0].req) == ">=1.0");
    CHECK(loaded.dependents[0].optional == 0);

    const KyubuModInfo& disabled = by_id.at("mm_disabled");
    CHECK(disabled.state == KY_MOD_STATE_DISABLED);
    CHECK(std::string(disabled.name) == "Manage: disabled");
    CHECK(std::string(disabled.version) == "0.9.0");
    CHECK(std::string(disabled.reason) == "disabled in kyubu/config.toml");   // spec: the manifest fields stay

    const KyubuModInfo& failed = by_id.at("mm_failed");
    CHECK(failed.state == KY_MOD_STATE_FAILED);
    CHECK(std::string(failed.name).empty());            // the manifest does not parse
    CHECK(std::string(failed.version).empty());
    CHECK(std::string(failed.reason).find("key 'version'") != std::string::npos);   // the log's own error text

    const KyubuModInfo& zipped = by_id.at("mm_zipped");
    CHECK(zipped.state == KY_MOD_STATE_LOADED);
    CHECK(zipped.is_zip == 1);
    CHECK(zipped.dependency_count == 1);
    CHECK(std::string(zipped.dependencies[0].id) == "mm_loaded");
    std::error_code ec;
    CHECK(zipped.package_size == fs::file_size(P(g.l.mods_dir + "/mm_zipped.zip"), ec));

    CHECK(by_id.at("mm_restricted").state == KY_MOD_STATE_LOADED);

    // the bounds and the arguments
    KyubuModInfo info{};
    info.size = sizeof info;
    CHECK(api->mods_get(caller->handle(KY_SIDE_CLIENT), 5, &info) == KY_E_NOT_FOUND);
    CHECK(api->mods_get(caller->handle(KY_SIDE_CLIENT), 0, nullptr) == KY_E_INVALID_ARG);
    info.size = offsetof(KyubuModInfo, dependencies);   // an older struct ends at `path`: still described
    CHECK(api->mods_get(caller->handle(KY_SIDE_CLIENT), 0, &info) == KY_OK);
    info.size = 4;                                      // too small even for the counts: refused
    CHECK(api->mods_get(caller->handle(KY_SIDE_CLIENT), 0, &info) == KY_E_INVALID_ARG);

    // the permission: a restricted mod is denied, and nothing is listed for it
    Mod* restricted_mod = g.mm.find("mm_restricted");
    REQUIRE(restricted_mod);
    CHECK(api->mods_count(restricted_mod->handle(KY_SIDE_CLIENT)) == 0);
    CHECK(api->mods_get(restricted_mod->handle(KY_SIDE_CLIENT), 0, &info) == KY_E_DENIED);
    info.size = sizeof info;
    CHECK(api->mods_get(restricted_mod->handle(KY_SIDE_CLIENT), 0, &info) == KY_E_DENIED);
}

TEST_CASE("mod-management: the package hash follows the D5 rule and the golden vector") {
    // covers: design.md D5 "Package hash" (the fixture's shared vector, zip branch, cache)
    ManageGame g("hash");
    const KyubuApi* api = api_table();
    Mod* caller = g.mm.find("mm_loaded");
    REQUIRE(caller);

    // parse golden.txt: the listing lines and the digest of the listing text
    const std::string golden = text_of2(kMfx + "/golden.txt");
    const size_t listing_at = golden.find("listing:\n");
    const size_t folder_at = golden.find("\nfolder:");
    REQUIRE(listing_at != std::string::npos);
    REQUIRE(folder_at != std::string::npos);
    const std::string listing = golden.substr(listing_at + 9, folder_at - (listing_at + 9) + 1);   // keeps its "\n"s
    size_t value_at = golden.find_first_not_of("\n", folder_at + 8);
    REQUIRE(value_at != std::string::npos);
    const std::string folder_hex = golden.substr(value_at, 64);

    // the listing text hashes to the folder digest
    uint8_t of_listing[32];
    {
        Sha256 s;
        s.update(listing.data(), listing.size());
        s.final(of_listing);
    }
    uint8_t golden_digest[32];
    REQUIRE(unhex(folder_hex, golden_digest));
    CHECK(Sha256::to_hex(of_listing) == folder_hex);

    // the loader computes the same digest
    uint8_t digest[32] = {};
    CHECK(api->mods_package_hash(caller->handle(KY_SIDE_CLIENT), "mm_loaded", digest) == KY_OK);
    CHECK(same_digest(digest, golden_digest));
    // and caches it: a second call with nothing changed gives the same bytes
    uint8_t again[32];
    CHECK(api->mods_package_hash(caller->handle(KY_SIDE_CLIENT), "mm_loaded", again) == KY_OK);
    CHECK(same_digest(again, digest));

    // a zip hashes its whole file (built at test time, so the test computes its digest too)
    auto zip_bytes = read_file(g.l.mods_dir + "/mm_zipped.zip");
    REQUIRE(zip_bytes.has_value());
    uint8_t zip_digest[32];
    {
        Sha256 s;
        s.update(zip_bytes->data(), zip_bytes->size());
        s.final(zip_digest);
    }
    CHECK(api->mods_package_hash(caller->handle(KY_SIDE_CLIENT), "mm_zipped", digest) == KY_OK);
    CHECK(same_digest(digest, zip_digest));

    // a disabled package still hashes
    CHECK(api->mods_package_hash(caller->handle(KY_SIDE_CLIENT), "mm_disabled", digest) == KY_OK);

    // unknown id, NULL arguments
    CHECK(api->mods_package_hash(caller->handle(KY_SIDE_CLIENT), "no_such_mod", digest) == KY_E_NOT_FOUND);
    CHECK(api->mods_package_hash(caller->handle(KY_SIDE_CLIENT), nullptr, digest) == KY_E_INVALID_ARG);
    CHECK(api->mods_package_hash(caller->handle(KY_SIDE_CLIENT), "mm_loaded", nullptr) == KY_E_INVALID_ARG);
}

// ----------------------------------------------------------------- 3.3: the options of any mod
TEST_CASE("mod-management: the options of any mod") {
    // covers: mod-management/The options of any mod, Label group and restart,
    // Setting any mod's option now (loaded and disabled, the created file)
    ManageGame g("options");
    const KyubuApi* api = api_table();
    Mod* caller = g.mm.find("mm_loaded");
    REQUIRE(caller);
    const KyubuModHandle h = caller->handle(KY_SIDE_CLIENT);

    // the fixture keeps the D5 golden vector clean, so the loaded mod's options are written at test time
    write_of(g.l.mods_dir + "/mm_loaded/config.toml", R"([options.ring_alpha]
type = "float"
default = 0.8
min = 0.0
max = 1.0
label = "Ring opacity"
group = "Visual"
description = "The ring's opacity."

[options.element]
type = "string"
default = "fire"
choices = ["fire", "ice"]
group = "Combat"

[options.shine]
type = "bool"
default = false
restart = true
)");
    std::string attach_error;
    REQUIRE(config_attach(caller, join_path(g.l.kyubu_dir, "config"), &attach_error));
    CHECK(attach_error.empty());
    REQUIRE(caller->config);   // a loaded mod: its own ModConfig answers
    make_dirs(join_path(g.l.kyubu_dir, "config"));

    // the declarations: file order, all fields, the value in use = the default of a fresh file
    CHECK(api->mods_option_count(h, "mm_loaded") == 3);
    KyubuOptionInfo o{};
    REQUIRE(api->mods_option_get(h, "mm_loaded", 0, &o) == KY_OK);
    CHECK(std::string(o.key) == "ring_alpha");
    CHECK(std::string(o.type) == "float");
    CHECK(std::string(o.label) == "Ring opacity");
    CHECK(std::string(o.group) == "Visual");
    CHECK(std::string(o.description) == "The ring's opacity.");
    CHECK(std::string(o.def) == "0.8");
    CHECK(std::string(o.value) == "0.8");
    CHECK(std::string(o.min) == "0.0");
    CHECK(std::string(o.max) == "1.0");
    CHECK(std::string(o.choices).empty());
    CHECK(o.restart == 0);

    REQUIRE(api->mods_option_get(h, "mm_loaded", 1, &o) == KY_OK);
    CHECK(std::string(o.key) == "element");
    CHECK(std::string(o.type) == "string");
    CHECK(std::string(o.def) == "\"fire\"");
    CHECK(std::string(o.choices) == "fire, ice");
    CHECK(std::string(o.min).empty());
    CHECK(std::string(o.max).empty());

    REQUIRE(api->mods_option_get(h, "mm_loaded", 2, &o) == KY_OK);
    CHECK(std::string(o.key) == "shine");
    CHECK(o.restart == 1);     // D14: takes effect at the next start, still listed and settable

    CHECK(api->mods_option_get(h, "mm_loaded", 3, &o) == KY_E_NOT_FOUND);
    CHECK(api->mods_option_get(h, "mm_loaded", 0, nullptr) == KY_E_INVALID_ARG);
    CHECK(api->mods_option_get(h, "no_such_mod", 0, &o) == KY_E_NOT_FOUND);
    CHECK(api->mods_option_count(h, "mm_failed") == 0);   // no config.toml in the fixture

    // the options of a disabled mod: the declaration parsed on request, the player's file value in use
    CHECK(api->mods_option_count(h, "mm_disabled") == 2);
    const std::string player_file = join_path(join_path(g.l.kyubu_dir, "config"), "mm_disabled.toml");
    write_of(player_file, "radius = 12.0\n");
    REQUIRE(api->mods_option_get(h, "mm_disabled", 0, &o) == KY_OK);
    CHECK(std::string(o.key) == "radius");
    CHECK(std::string(o.value) == "12.0");
    CHECK(std::string(o.label) == "Ring brightness");
    CHECK(std::string(o.group) == "Ring");

    // without the player's file the default stands
    fs::remove(fs::path(widen(player_file)));
    REQUIRE(api->mods_option_get(h, "mm_disabled", 0, &o) == KY_OK);
    CHECK(std::string(o.value) == "8.0");

    // mods_option_check: the same rules as the player's file, nothing saved
    write_of(player_file, "radius = 12.0\n");
    const std::string before = text_of2(player_file);
    const uint64_t seq = Log::get().sequence();
    CHECK(api->mods_option_check(h, "mm_disabled", "radius", "100") == KY_E_INVALID_ARG);
    CHECK(logged(seq, "mods_option_check: radius = 100 is not allowed (a number from 1.0 to 32.0)"));
    CHECK(api->mods_option_check(h, "mm_disabled", "radius", "abc") == KY_E_INVALID_ARG);
    CHECK(api->mods_option_check(h, "mm_disabled", "radius", "12.5") == KY_OK);
    CHECK(text_of2(player_file) == before);
    CHECK(api->mods_option_check(h, "mm_disabled", "nonesuch", "1") == KY_E_NOT_FOUND);
    CHECK(api->mods_option_check(h, "mm_loaded", "element", "steam") == KY_E_INVALID_ARG);   // not a choice

    // a loaded mod is set exactly as its own config_set: saved, config.changed queued for the next frame
    Loader& l = g.l;
    const auto saved_phase = l.phase.load();
    l.phase = KY_PHASE_GAMEREADY;
    std::vector<std::string> seen;
    const KyubuId sub = Events::get().subscribe_internal("config.changed", KY_SIDE_CLIENT, 0,
                                                         [&](KyubuEvent* ev) {
        auto d = (KyubuConfigChangedData*)ev->data;
        seen.push_back(std::string(d->mod) + "." + std::string(d->key));
    });
    const uint64_t seq2 = Log::get().sequence();
    CHECK(api->mods_option_set(h, "mm_loaded", "ring_alpha", "0.6") == KY_OK);
    CHECK(text_of2(join_path(join_path(g.l.kyubu_dir, "config"), "mm_loaded.toml")).find("ring_alpha = 0.6") !=
          std::string::npos);
    CHECK(api->mods_option_set(h, "mm_loaded", "ring_alpha", "3.0") == KY_E_INVALID_ARG);   // above the max
    CHECK(logged(seq2, "mods_option_set: ring_alpha = 3.0 is not allowed (a number from 0.0 to 1.0)"));
    config_pump();
    CHECK(seen == std::vector<std::string>{"mm_loaded.ring_alpha"});
    Events::get().unsubscribe(nullptr, sub);
    l.phase = saved_phase;

    // a disabled mod's set: the file is written, created with the defaults, and the mod stays disabled
    fs::remove(fs::path(widen(player_file)));
    CHECK(api->mods_option_set(h, "mm_disabled", "radius", "12.0") == KY_OK);
    CHECK(g.mm.find("mm_disabled")->state == ModState::Disabled);   // changing its options loads nothing
    const std::string created = text_of2(player_file);
    CHECK(created.find("radius = 12.0") != std::string::npos);
    CHECK(created.find("shine = false") != std::string::npos);      // the defaults came along
    CHECK(api->mods_option_set(h, "mm_disabled", "radius", "24.0") == KY_OK);
    CHECK(text_of2(player_file).find("radius = 24.0") != std::string::npos);
    CHECK(api->mods_option_set(h, "mm_disabled", "radius", "0") == KY_E_INVALID_ARG);   // below the min

    // the permission: a restricted caller is denied (count 0) on every member
    Mod* restricted_caller = g.mm.find("mm_restricted");
    REQUIRE(restricted_caller);
    CHECK(api->mods_option_count(restricted_caller->handle(KY_SIDE_CLIENT), "mm_loaded") == 0);
    CHECK(api->mods_option_get(restricted_caller->handle(KY_SIDE_CLIENT), "mm_loaded", 0, &o) == KY_E_DENIED);
    CHECK(api->mods_option_check(restricted_caller->handle(KY_SIDE_CLIENT), "mm_loaded", "ring_alpha", "0.5") ==
          KY_E_DENIED);
    CHECK(api->mods_option_set(restricted_caller->handle(KY_SIDE_CLIENT), "mm_loaded", "ring_alpha", "0.5") ==
          KY_E_DENIED);
    // denied: the file keeps its value
    CHECK(text_of2(join_path(join_path(g.l.kyubu_dir, "config"), "mm_loaded.toml")).find("ring_alpha = 0.6") !=
          std::string::npos);
}

// ----------------------------------------------------------------- 3.3: the rescan
TEST_CASE("mod-management: mods_rescan lists added and removed packages") {
    // covers: mod-management/Rescanning the mods folder (added, failed to parse, removed)
    ManageGame g("rescan");
    const KyubuApi* api = api_table();
    Mod* caller = g.mm.find("mm_loaded");
    REQUIRE(caller);
    const KyubuModHandle h = caller->handle(KY_SIDE_CLIENT);

    // the boot listing first
    CHECK(api->mods_count(h) == 5);
    KyubuModInfo info{};
    info.size = sizeof info;
    CHECK(api->mods_get(h, 0, &info) == KY_OK);   // discover order, unchanged

    // a package dropped in while the game runs: the state `new`, its manifest read, nothing loaded
    fs::create_directories(fs::path(widen(g.l.mods_dir + "/mm_added")));
    write_of(g.l.mods_dir + "/mm_added/mod.toml", R"(id = "mm_added"
name = "Manage: added"
version = "1.0.0"
kyubu = "^1.2"
sides = ["client"]
)");
    CHECK(api->mods_rescan(h) == KY_OK);
    CHECK(api->mods_count(h) == 6);

    auto find_listed = [&](const char* id) -> KyubuModInfo {
        KyubuModInfo found{};
        found.size = sizeof found;
        for (uint32_t i = 0, n = api->mods_count(h); i < n; ++i) {
            KyubuModInfo one{};
            one.size = sizeof one;
            REQUIRE(api->mods_get(h, i, &one) == KY_OK);
            if (std::string(one.id) == id) found = one;
        }
        return found;
    };
    const KyubuModInfo added = find_listed("mm_added");
    CHECK(added.state == KY_MOD_STATE_NEW);
    CHECK(std::string(added.name) == "Manage: added");
    CHECK(std::string(added.version) == "1.0.0");
    CHECK(std::string(added.path).find("mm_added") != std::string::npos);

    // a package whose mod.toml does not parse: the state `failed`, the error as its reason
    fs::create_directories(fs::path(widen(g.l.mods_dir + "/mm_broken")));
    write_of(g.l.mods_dir + "/mm_broken/mod.toml", "id = \"mm_broken\"\nname = \"Manage: broken\"\nversion = 1.2\n");
    CHECK(api->mods_rescan(h) == KY_OK);
    CHECK(api->mods_count(h) == 7);
    const KyubuModInfo broken = find_listed("mm_broken");
    CHECK(broken.state == KY_MOD_STATE_FAILED);
    CHECK(std::string(broken.name).empty());
    CHECK(std::string(broken.reason).find("key 'version'") != std::string::npos);
    CHECK(std::string(broken.id) == "mm_broken");   // the folder name stands for the id

    // a package removed by hand: the state `missing`, the other fields stay
    fs::remove(fs::path(widen(g.l.mods_dir + "/mm_zipped.zip")));
    CHECK(api->mods_rescan(h) == KY_OK);
    CHECK(api->mods_count(h) == 7);               // it is still listed, nothing is unloaded
    const KyubuModInfo gone = find_listed("mm_zipped");
    CHECK(gone.state == KY_MOD_STATE_MISSING);
    CHECK(std::string(gone.name) == "Manage: zipped");
    CHECK(g.mm.find("mm_loaded")->state == ModState::Loaded);
    CHECK(!fs::exists(fs::path(widen(join_path(join_path(g.l.kyubu_dir, "config"), "mm_disabled.toml")))));

    // a second rescan changes nothing
    CHECK(api->mods_rescan(h) == KY_OK);
    CHECK(api->mods_count(h) == 7);

    // the permission: a restricted caller is denied
    Mod* restricted_caller = g.mm.find("mm_restricted");
    REQUIRE(restricted_caller);
    CHECK(api->mods_rescan(restricted_caller->handle(KY_SIDE_CLIENT)) == KY_E_DENIED);
}

TEST_CASE("mod-management: mods_queue validates and writes kyubu\\pending.toml") {
    ManageGame g("queue");
    const std::string pending = join_path(g.l.kyubu_dir, "pending.toml");

    // a value the option refuses: KY_E_INVALID_ARG, no pending file, no options file
    KyubuModOp bad = make_op(KY_MOD_OP_CONFIG, "mm_disabled");
    set_key_value(bad, "radius", "100");
    const uint64_t seq = Log::get().sequence();
    CHECK(manage_queue(&bad) == KY_E_INVALID_ARG);
    CHECK_FALSE(file_exists(pending));
    CHECK_FALSE(file_exists(join_path(join_path(g.l.kyubu_dir, "config"), "mm_disabled.toml")));
    CHECK(logged(seq, "mods_queue"));

    // a value of the wrong type, an unknown option, a package nobody has
    KyubuModOp wrong_type = make_op(KY_MOD_OP_CONFIG, "mm_disabled");
    set_key_value(wrong_type, "radius", "wide");
    CHECK(manage_queue(&wrong_type) == KY_E_INVALID_ARG);
    KyubuModOp unknown_key = make_op(KY_MOD_OP_CONFIG, "mm_disabled");
    set_key_value(unknown_key, "nope", "1");
    CHECK(manage_queue(&unknown_key) == KY_E_NOT_FOUND);
    KyubuModOp unknown_pkg = make_op(KY_MOD_OP_CONFIG, "mm_nope");
    set_key_value(unknown_pkg, "radius", "12.5");
    CHECK(manage_queue(&unknown_pkg) == KY_E_NOT_FOUND);

    // install: the version must be semver and the package inside kyubu\staging or kyubu\backup
    KyubuModOp bad_version = make_op(KY_MOD_OP_INSTALL, "mm_new");
    set_version(bad_version, "one");
    set_from(bad_version, "kyubu/staging/mm_new-1.0.0.zip");
    CHECK(manage_queue(&bad_version) == KY_E_INVALID_ARG);
    KyubuModOp escape = make_op(KY_MOD_OP_INSTALL, "mm_new");
    set_version(escape, "1.0.0");
    set_from(escape, "kyubu/staging/../pending.toml");
    CHECK(manage_queue(&escape) == KY_E_INVALID_ARG);
    KyubuModOp outside = make_op(KY_MOD_OP_INSTALL, "mm_new");
    set_version(outside, "1.0.0");
    set_from(outside, "mods/mm_loaded");
    CHECK(manage_queue(&outside) == KY_E_INVALID_ARG);
    CHECK_FALSE(file_exists(pending));

    // valid ops: written in full and in queue order, and nothing else changes
    KyubuModOp disable = make_op(KY_MOD_OP_DISABLE, "mm_loaded");
    CHECK(manage_queue(&disable) == KY_OK);
    REQUIRE(file_exists(pending));
    KyubuModOp radius = make_op(KY_MOD_OP_CONFIG, "mm_disabled");
    set_key_value(radius, "radius", "12.5");
    CHECK(manage_queue(&radius) == KY_OK);
    CHECK_FALSE(file_exists(g.l.config_path));
    CHECK_FALSE(file_exists(join_path(join_path(g.l.kyubu_dir, "config"), "mm_disabled.toml")));

    const std::string text = text_of2(pending);
    CHECK(text.find("op = [") != std::string::npos);
    auto parsed = toml::parse(text, pending);
    REQUIRE(parsed);
    auto arr = parsed.table()["op"].as_array();
    REQUIRE(arr);
    REQUIRE(arr->size() == 2);
    auto r0 = (*arr)[0].as_table();
    auto r1 = (*arr)[1].as_table();
    REQUIRE(r0);
    REQUIRE(r1);
    CHECK((*r0)["kind"].value_or(std::string()) == "disable");
    CHECK((*r0)["id"].value_or(std::string()) == "mm_loaded");
    CHECK((*r1)["kind"].value_or(std::string()) == "config");
    CHECK((*r1)["key"].value_or(std::string()) == "radius");
    CHECK((*r1)["value"].value_or(std::string()) == "12.5");

    CHECK(manage_queue_count() == 2);
    KyubuModOp first{};
    CHECK(manage_queue_get(0, &first) == KY_OK);
    CHECK(first.kind == KY_MOD_OP_DISABLE);
    CHECK(std::string(first.id) == "mm_loaded");
    KyubuModOp second{};
    CHECK(manage_queue_get(1, &second) == KY_OK);
    CHECK(second.kind == KY_MOD_OP_CONFIG);
    CHECK(std::string(second.key) == "radius");
    CHECK(std::string(second.value) == "12.5");
    CHECK(manage_queue_get(2, &second) == KY_E_NOT_FOUND);
    CHECK(manage_queue_get(0, nullptr) == KY_E_INVALID_ARG);

    // the permission covers the queue members too
    Mod* restricted = g.mm.find("mm_restricted");
    REQUIRE(restricted);
    const KyubuApi* api = api_table();
    KyubuModHandle h = restricted->handle(KY_SIDE_CLIENT);
    CHECK(api->mods_queue(h, &disable) == KY_E_DENIED);
    CHECK(api->mods_queue_clear(h) == KY_E_DENIED);
    CHECK(api->mods_queue_count(h) == 0);
    CHECK(api->mods_queue_get(h, 0, &second) == KY_E_DENIED);
    CHECK(manage_queue_count() == 2);   // nothing of that reached the queue

    CHECK(manage_queue_clear() == KY_OK);
    CHECK_FALSE(file_exists(pending));
    CHECK(manage_queue_count() == 0);
}

TEST_CASE("mod-management: the queue applies at boot: installs, backups, disabled options, last-apply") {
    ManageGame g("apply");
    const std::string staging = join_path(g.l.kyubu_dir, "staging");
    const std::string backup = join_path(g.l.kyubu_dir, "backup");
    const std::string config_dir = join_path(g.l.kyubu_dir, "config");
    make_dirs(staging);

    // two staged packages, and a file no op names (deleted at boot)
    staged_zip(join_path(staging, "mm_new-1.0.0.zip"),
               "id = \"mm_new\"\nname = \"Manage: new\"\nversion = \"1.0.0\"\nkyubu = \"^1.1\"\nsides = [\"client\"]\n",
               "[options.shine]\ntype = \"bool\"\ndefault = true\n");
    staged_zip(join_path(staging, "mm_loaded-1.1.0.zip"),
               "id = \"mm_loaded\"\nname = \"Manage: loaded\"\nversion = \"1.1.0\"\nkyubu = \"^1.1\"\nsides = [\"client\"]\nmanage_mods = true\n",
               "");
    write_of(join_path(staging, "stray-0.1.0.zip"), "not a package");

    // a config.toml whose [mods] last-apply.toml has to keep as the previous state
    write_of(g.l.config_path, "[loader]\ndev_mode = false\n\n[mods.mm_other]\nenabled = false\n");

    KyubuModOp install_new = make_op(KY_MOD_OP_INSTALL, "mm_new");
    set_version(install_new, "1.0.0");
    set_from(install_new, "kyubu/staging/mm_new-1.0.0.zip");
    CHECK(manage_queue(&install_new) == KY_OK);
    // a config op for a mod only the queue installs: its declaration comes from the staged package
    KyubuModOp shine = make_op(KY_MOD_OP_CONFIG, "mm_new");
    set_key_value(shine, "shine", "false");
    CHECK(manage_queue(&shine) == KY_OK);
    KyubuModOp install_loaded = make_op(KY_MOD_OP_INSTALL, "mm_loaded");
    set_version(install_loaded, "1.1.0");
    set_from(install_loaded, "kyubu/staging/mm_loaded-1.1.0.zip");
    CHECK(manage_queue(&install_loaded) == KY_OK);
    KyubuModOp disable_loaded = make_op(KY_MOD_OP_DISABLE, "mm_loaded");
    CHECK(manage_queue(&disable_loaded) == KY_OK);
    KyubuModOp disable_disabled = make_op(KY_MOD_OP_DISABLE, "mm_disabled");
    CHECK(manage_queue(&disable_disabled) == KY_OK);
    KyubuModOp radius = make_op(KY_MOD_OP_CONFIG, "mm_disabled");
    set_key_value(radius, "radius", "12.5");
    CHECK(manage_queue(&radius) == KY_OK);

    // queued: nothing has changed yet
    CHECK(dir_exists(join_path(g.l.mods_dir, "mm_loaded")));
    CHECK_FALSE(file_exists(join_path(g.l.mods_dir, "mm_loaded-1.1.0.zip")));
    CHECK(g.l.config.is_enabled("mm_loaded"));

    const PendingResult r = apply_pending();
    CHECK(r.applied == 6);
    CHECK(r.failed == 0);
    CHECK_FALSE(r.skipped);
    CHECK(pending_notice(r).empty());
    CHECK_FALSE(file_exists(join_path(g.l.kyubu_dir, "pending.toml")));

    // the install replaced the folder and kept it in the backup (spec: update replaces a package)
    CHECK_FALSE(dir_exists(join_path(g.l.mods_dir, "mm_loaded")));
    CHECK(file_exists(join_path(g.l.mods_dir, "mm_loaded-1.1.0.zip")));
    CHECK(dir_exists(join_path(backup, "mm_loaded-1.0.0")));
    CHECK(file_exists(join_path(g.l.mods_dir, "mm_new-1.0.0.zip")));

    // both now disabled, and the scan after this reads it
    CHECK_FALSE(g.l.config.is_enabled("mm_loaded"));
    CHECK_FALSE(g.l.config.is_enabled("mm_disabled"));
    CHECK(text_of2(g.l.config_path).find("mm_loaded") != std::string::npos);

    // an option of a mod that stays disabled: the file with every default and the value
    const std::string disabled_opts = text_of2(join_path(config_dir, "mm_disabled.toml"));
    CHECK(disabled_opts.find("radius = 12.5") != std::string::npos);
    CHECK(disabled_opts.find("shine = false") != std::string::npos);
    CHECK(text_of2(join_path(config_dir, "mm_new.toml")).find("shine = false") != std::string::npos);

    // staging: the unnamed file is gone, the installed one moved into the mods folder
    CHECK_FALSE(file_exists(join_path(staging, "stray-0.1.0.zip")));
    CHECK_FALSE(file_exists(join_path(staging, "mm_new-1.0.0.zip")));
    CHECK_FALSE(file_exists(join_path(staging, "mm_loaded-1.1.0.zip")));

    // last-apply.toml: the previous [mods] (mm_loaded was not in it yet) and the package it moved
    const std::string undo = text_of2(join_path(backup, "last-apply.toml"));
    CHECK(undo.find("mm_other") != std::string::npos);
    CHECK(undo.find("[mods.mm_loaded]") == std::string::npos);
    CHECK(undo.find("kyubu/backup/mm_loaded-1.0.0") != std::string::npos);
}

TEST_CASE("mod-management: a config op is checked against a queued install of the same id, not the installed package") {
    // add-mod-manager task 8.2: a modpack replaces mm_loaded 1.0.0 (no options) with 2.0.0, which declares `boost`
    ManageGame g("config-new-version");
    const std::string staging = join_path(g.l.kyubu_dir, "staging");
    make_dirs(staging);
    staged_zip(join_path(staging, "mm_loaded-2.0.0.zip"),
               "id = \"mm_loaded\"\nname = \"Manage: loaded\"\nversion = \"2.0.0\"\nkyubu = \"^1.1\"\nsides = [\"client\"]\nmanage_mods = true\n",
               "[options.boost]\ntype = \"int\"\ndefault = 1\nmin = 0\nmax = 5\n");
    KyubuModOp boost = make_op(KY_MOD_OP_CONFIG, "mm_loaded");
    set_key_value(boost, "boost", "3");
    CHECK(manage_queue(&boost) == KY_E_NOT_FOUND);   // the installed 1.0.0 declares no options
    KyubuModOp install = make_op(KY_MOD_OP_INSTALL, "mm_loaded");
    set_version(install, "2.0.0");
    set_from(install, "kyubu/staging/mm_loaded-2.0.0.zip");
    CHECK(manage_queue(&install) == KY_OK);
    CHECK(manage_queue(&boost) == KY_OK);
    KyubuModOp too_big = make_op(KY_MOD_OP_CONFIG, "mm_loaded");
    set_key_value(too_big, "boost", "9");
    CHECK(manage_queue(&too_big) == KY_E_INVALID_ARG);   // the staged declaration's limits apply
    const PendingResult r = apply_pending();
    CHECK(r.applied == 2);
    CHECK(r.failed == 0);
    const std::string written = text_of2(join_path(join_path(g.l.kyubu_dir, "config"), "mm_loaded.toml"));
    CHECK(written.find("boost = 3") != std::string::npos);
}

TEST_CASE("mod-management: delete with and without settings, a failed op, the notice, the purge") {
    ManageGame g("delete");
    const std::string config_dir = join_path(g.l.kyubu_dir, "config");
    const std::string data_dir = join_path(g.l.kyubu_dir, "data");
    const std::string backup = join_path(g.l.kyubu_dir, "backup");
    make_dirs(config_dir);
    make_dirs(data_dir);
    make_dirs(backup);
    write_of(join_path(config_dir, "mm_disabled.toml"), "radius = 9.0\n");
    write_of(join_path(config_dir, "mm_restricted.toml"), "radius = 4.0\n");
    write_of(join_path(data_dir, "mm_restricted/notes.txt"), "the data");
    write_of(join_path(config_dir, "mm_zipped.toml"), "kept = true\n");
    write_of(join_path(data_dir, "mm_zipped/notes.txt"), "kept data");
    write_of(join_path(backup, "zlib1.dll"), "the original library");

    // the options file as it was before the config op, for last-apply.toml
    KyubuModOp config_op = make_op(KY_MOD_OP_CONFIG, "mm_disabled");
    set_key_value(config_op, "radius", "20.0");
    CHECK(manage_queue(&config_op) == KY_OK);
    KyubuModOp del_restricted = make_op(KY_MOD_OP_DELETE, "mm_restricted");
    del_restricted.keep_settings = 0;
    CHECK(manage_queue(&del_restricted) == KY_OK);
    KyubuModOp del_zipped = make_op(KY_MOD_OP_DELETE, "mm_zipped");
    del_zipped.keep_settings = 1;
    CHECK(manage_queue(&del_zipped) == KY_OK);
    // an install whose staged file is missing: queued, failed at boot, the other ops still run
    KyubuModOp missing = make_op(KY_MOD_OP_INSTALL, "mm_gone");
    set_version(missing, "1.0.0");
    set_from(missing, "kyubu/staging/mm_gone-1.0.0.zip");
    CHECK(manage_queue(&missing) == KY_OK);

    const PendingResult r = apply_pending();
    CHECK(r.applied == 3);
    CHECK(r.failed == 1);
    CHECK(pending_notice(r) == "ModManager: 1 of 4 changes failed, see the log");
    CHECK_FALSE(file_exists(join_path(g.l.kyubu_dir, "pending.toml")));

    // delete without settings: the package in the backup, the options file and the data gone
    CHECK_FALSE(dir_exists(join_path(g.l.mods_dir, "mm_restricted")));
    CHECK(dir_exists(join_path(backup, "mm_restricted-1.0.0")));
    CHECK_FALSE(file_exists(join_path(config_dir, "mm_restricted.toml")));
    CHECK_FALSE(dir_exists(join_path(data_dir, "mm_restricted")));

    // delete with settings: the zip in the backup, the files stay
    CHECK_FALSE(file_exists(join_path(g.l.mods_dir, "mm_zipped.zip")));
    CHECK(file_exists(join_path(backup, "mm_zipped-2.0.0.zip")));
    CHECK(text_of2(join_path(config_dir, "mm_zipped.toml")).find("kept = true") != std::string::npos);
    CHECK(file_exists(join_path(data_dir, "mm_zipped/notes.txt")));

    // the option was written after the delete pass, and last-apply.toml holds its previous text
    CHECK(text_of2(join_path(config_dir, "mm_disabled.toml")).find("radius = 20.0") != std::string::npos);
    const std::string undo = text_of2(join_path(backup, "last-apply.toml"));
    CHECK(undo.find("radius = 9.0") != std::string::npos);
    CHECK(undo.find("mm_restricted-1.0.0") != std::string::npos);

    // a boot with no pending file: the backup packages go, other files there stay
    const PendingResult again = apply_pending();
    CHECK(again.applied == 0);
    CHECK(again.failed == 0);
    CHECK_FALSE(dir_exists(join_path(backup, "mm_restricted-1.0.0")));
    CHECK_FALSE(file_exists(join_path(backup, "mm_zipped-2.0.0.zip")));
    CHECK_FALSE(file_exists(join_path(backup, "last-apply.toml")));
    CHECK(file_exists(join_path(backup, "zlib1.dll")));
}

TEST_CASE("mod-management: the spec's order puts installs before deletes") {
    ManageGame g("order");
    const std::string staging = join_path(g.l.kyubu_dir, "staging");
    const std::string backup = join_path(g.l.kyubu_dir, "backup");
    make_dirs(staging);
    staged_zip(join_path(staging, "mm_disabled-0.9.0.zip"),
               "id = \"mm_disabled\"\nname = \"Manage: disabled\"\nversion = \"0.9.0\"\nkyubu = \"^1.1\"\nsides = [\"client\"]\n",
               "");

    // queued delete first, install second: at boot the install runs first, then the delete takes the new zip
    KyubuModOp del = make_op(KY_MOD_OP_DELETE, "mm_disabled");
    CHECK(manage_queue(&del) == KY_OK);
    KyubuModOp install = make_op(KY_MOD_OP_INSTALL, "mm_disabled");
    set_version(install, "0.9.0");
    set_from(install, "kyubu/staging/mm_disabled-0.9.0.zip");
    CHECK(manage_queue(&install) == KY_OK);

    const PendingResult r = apply_pending();
    CHECK(r.applied == 2);
    CHECK_FALSE(dir_exists(join_path(g.l.mods_dir, "mm_disabled")));
    CHECK_FALSE(file_exists(join_path(g.l.mods_dir, "mm_disabled-0.9.0.zip")));
    CHECK(dir_exists(join_path(backup, "mm_disabled-0.9.0")));         // the folder install replaced
    CHECK(file_exists(join_path(backup, "mm_disabled-0.9.0.zip")));    // the zip the delete took
}

TEST_CASE("mod-management: safe mode keeps the pending file and applies nothing") {
    ManageGame g("safe");
    KyubuModOp disable = make_op(KY_MOD_OP_DISABLE, "mm_loaded");
    CHECK(manage_queue(&disable) == KY_OK);

    g.l.safe_mode = true;
    const PendingResult r = apply_pending();
    CHECK(r.skipped);
    CHECK(r.applied == 0);
    CHECK(r.failed == 0);
    CHECK(file_exists(join_path(g.l.kyubu_dir, "pending.toml")));
    CHECK_FALSE(file_exists(g.l.config_path));

    // the next normal start applies it
    g.l.safe_mode = false;
    const PendingResult again = apply_pending();
    CHECK(again.applied == 1);
    CHECK_FALSE(file_exists(join_path(g.l.kyubu_dir, "pending.toml")));
    CHECK_FALSE(g.l.config.is_enabled("mm_loaded"));
}

TEST_CASE("mod-management: game_restart is denied without the flag and refused outside the start menu") {
    // covers: spec "Restart" (the refusals: nothing is started), "Mod management permission"
    Loader& l = L();
    const KyubuHost old_host = l.host.host;
    const KyubuPhase old_phase = l.phase.load();
    const KyubuApi* api = api_table();
    auto handle_of = [](DemoMod& d) {
        for (int s = 1; s <= 2; ++s) { d.mod.ctx[s].mod = &d.mod; d.mod.ctx[s].side = (KyubuSide)s; }
        return d.mod.handle(KY_SIDE_CLIENT);
    };
    DemoMod allowed("manager", "full", true);
    DemoMod no_flag("noperm", "full", false);
    DemoMod restricted("restricted", "restricted", true);
    const KyubuModHandle ha = handle_of(allowed);

    CHECK(api->game_restart(handle_of(no_flag)) == KY_E_DENIED);
    CHECK(api->game_restart(handle_of(restricted)) == KY_E_DENIED);

    // allowed, but every host check fails before anything is started: no pid, no environment variable
    std::string why;
    l.host.host = KY_HOST_SERVER;
    l.phase = KY_PHASE_GAMEREADY;
    CHECK(restart_game(&why) == KY_E_SIDE);
    l.host.host = KY_HOST_CLIENT;
    l.phase = KY_PHASE_REGISTER;
    CHECK(restart_game(&why) == KY_E_PHASE);
    CHECK(api->game_restart(ha) == KY_E_PHASE);
    l.phase = KY_PHASE_GAMEREADY;
    // no game controller in a test process: refused before CreateProcess
    CHECK(restart_game(&why) == KY_E_UNAVAILABLE);
    CHECK(why.find("game controller") != std::string::npos);
    CHECK(GetEnvironmentVariableA("KYUBU_RESTART_PARENT", nullptr, 0) == 0);

    // with a world loaded the call returns KY_E_PHASE and starts nothing
    char db_object[16] = {};
    void* const db_handle = reinterpret_cast<void*>(0x1000);
    std::memcpy(db_object + 4, &db_handle, sizeof db_handle);
    on_database_opened(db_object, "Save/world_restart_test.db", true);
    REQUIRE(world_loaded());
    CHECK(restart_game(&why) == KY_E_PHASE);
    CHECK(why.find("world is loaded") != std::string::npos);
    CHECK(api->game_restart(ha) == KY_E_PHASE);
    on_sqlite_closed(db_handle);
    CHECK_FALSE(world_loaded());
    CHECK(GetEnvironmentVariableA("KYUBU_RESTART_PARENT", nullptr, 0) == 0);

    // a process that was not restarted waits for nothing
    const RestartWait none = restart_wait_for_parent();
    CHECK_FALSE(none.restarted);

    // a restarted process waits for the parent's handle: here a process that has already ended
    SetEnvironmentVariableA("KYUBU_RESTART_PARENT", "0");
    const RestartWait bad = restart_wait_for_parent();   // handle 0 is not a process: the wait fails at once
    CHECK(bad.restarted);
    CHECK_FALSE(bad.ended);
    CHECK(bad.error != 0);
    CHECK(GetEnvironmentVariableA("KYUBU_RESTART_PARENT", nullptr, 0) == 0);   // removed for the next children

    l.host.host = old_host;
    l.phase = old_phase;
}
