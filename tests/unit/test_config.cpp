// mod-config (openspec/changes/add-mod-config): declared options, the player's file, validation, the API.
#include "doctest.h"

#include <filesystem>

#include "api.h"
#include "console.h"
#include "events.h"
#include "loader.h"
#include "log.h"
#include "luart.h"
#include "miniz.h"
#include "modconfig.h"
#include "modmanager.h"

using namespace kyubu;
namespace fs = std::filesystem;

namespace {

const std::string kFx = KYUBU_SOURCE_DIR "/tests/fixtures/config";

std::string text_of(const std::string& path) {
    auto d = read_file(path);
    return d ? std::string(d->begin(), d->end()) : std::string();
}

fs::path P(const std::string& utf8) { return fs::path(widen(utf8)); }

// A fresh folder under %TEMP% for one test.
std::string temp_dir(const char* name) {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    fs::path p = fs::path(tmp) / (std::string("kyubu-config-") + name + "-" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p);
    return narrow(p.wstring());
}

// A mod opened from a fixture folder (or zip), as mod loading sets it up.
struct FxMod {
    Mod m;
    FxMod(const std::string& fixture, const std::string& id, const std::string& path = "") {
        m.id = id;
        m.manifest.name = id;
        m.manifest.sides = KY_SIDE_CLIENT | KY_SIDE_SERVER;
        m.path = path.empty() ? kFx + "/" + fixture : path;
        m.pkg = Package::open(m.path);
        m.state = ModState::Loaded;
        for (int s = 1; s <= 2; ++s) { m.ctx[s].mod = &m; m.ctx[s].side = (KyubuSide)s; }
    }
    bool attach(const std::string& dir, std::string* err = nullptr) { return config_attach(&m, dir, err); }
    double radius() {
        OptValue v;
        REQUIRE(config_get(&m, "radius", OptType::Float, &v, nullptr) == KY_OK);
        return std::get<double>(v);
    }
};

std::string joined(const std::vector<std::string>& v) {
    std::string s;
    for (auto& x : v) s += x + "; ";
    return s;
}

std::string log_since(uint64_t seq) {
    std::string all;
    for (auto& l : Log::get().since(seq)) all += l.text + "\n";
    return all;
}

}  // namespace

// ------------------------------------------------------------------ declared options
TEST_CASE("mod-config: valid declaration") {
    // covers: mod-config/Valid declaration
    std::vector<std::string> errors;
    auto decls = parse_config_decl(text_of(kFx + "/cfg_demo/config.toml"), "mods/cfg_demo/config.toml", errors);
    REQUIRE(decls);
    CHECK(errors.empty());
    REQUIRE(decls->size() == 2);
    CHECK((*decls)[0].key == "radius");   // the order they are written in, not alphabetical
    CHECK((*decls)[1].key == "mode");
    CHECK((*decls)[0].type == OptType::Float);
    CHECK(std::get<double>(*(*decls)[0].min) == 1.0);
    CHECK((*decls)[1].choices == std::vector<std::string>{"fire", "ice"});

    FxMod f("cfg_demo", "cfg_demo");
    std::string dir = temp_dir("valid");
    REQUIRE(f.attach(dir));
    CHECK(f.radius() == 8.0);
}

TEST_CASE("mod-config: default of the wrong type") {
    // covers: mod-config/Default of the wrong type
    std::vector<std::string> errors;
    auto decls = parse_config_decl(text_of(kFx + "/cfg_bad_default/config.toml"), "mods/cfg_bad_default/config.toml", errors);
    CHECK_FALSE(decls);
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].find("mods/cfg_bad_default/config.toml") != std::string::npos);
    CHECK(errors[0].find("options.radius.default") != std::string::npos);
    CHECK(errors[0].find("type = \"int\"") != std::string::npos);
    CHECK(errors[0].find("got a string") != std::string::npos);

    FxMod f("cfg_bad_default", "cfg_bad_default");
    std::string dir = temp_dir("baddefault"), err;
    CHECK_FALSE(f.attach(dir, &err));
    CHECK(err.find("options.radius.default") != std::string::npos);
    CHECK_FALSE(file_exists(dir + "/cfg_bad_default.toml"));
}

TEST_CASE("mod-config: declaration errors name the key") {
    auto errs = [](const std::string& text) {
        std::vector<std::string> e;
        CHECK_FALSE(parse_config_decl(text, "config.toml", e));
        std::string all;
        for (auto& s : e) all += s + "\n";
        return all;
    };
    CHECK(errs("[options.Radius]\ntype = \"int\"\ndefault = 1\n").find("^[a-z][a-z0-9_]{0,31}$") != std::string::npos);
    CHECK(errs("[options.r]\ntype = \"list\"\ndefault = 1\n").find("options.r.type") != std::string::npos);
    CHECK(errs("[options.r]\ntype = \"int\"\n").find("options.r.default': required") != std::string::npos);
    CHECK(errs("[options.r]\ntype = \"int\"\ndefault = 1\ndefualt = 2\n").find("options.r.defualt': unknown field") != std::string::npos);
    CHECK(errs("[options.r]\ntype = \"int\"\ndefault = 50\nmax = 10\n").find("options.r.default': 50 is not an integer of at most 10") != std::string::npos);
    CHECK(errs("[options.r]\ntype = \"bool\"\ndefault = true\nmin = 1\n").find("only int and float") != std::string::npos);
    CHECK(errs("[options.r]\ntype = \"string\"\ndefault = \"x\"\nchoices = [\"a\"]\n").find("is not one of \"a\"") != std::string::npos);
    CHECK(errs("[options.r]\ntype = \"int\"\ndefault = 1.5\n").find("got a float") != std::string::npos);
    CHECK(errs("other = 1\n").find("key 'other': unknown key") != std::string::npos);
    // the settings-form fields (1.2): label at most 40 characters, group text, restart bool
    CHECK(errs("[options.r]\ntype = \"float\"\ndefault = 1.0\nlabel = 7\n").find("options.r.label': expected a string") != std::string::npos);
    const std::string long_label =
        "[options.r]\ntype = \"float\"\ndefault = 1.0\nlabel = \"" + std::string(41, 'x') + "\"\n";
    CHECK(errs(long_label).find("at most 40 characters") != std::string::npos);
    // the description is the hover tooltip of the Mods page: at most 120 characters
    CHECK(errs("[options.r]\ntype = \"float\"\ndefault = 1.0\ndescription = \"" + std::string(121, 'x') + "\"\n").find("at most 120") != std::string::npos);
    std::vector<std::string> e_desc;
    CHECK(parse_config_decl("[options.r]\ntype = \"float\"\ndefault = 1.0\ndescription = \"" + std::string(120, 'x') + "\"\n", "config.toml", e_desc));
    CHECK(errs("[options.r]\ntype = \"float\"\ndefault = 1.0\ngroup = 1\n").find("options.r.group': expected a string") != std::string::npos);
    CHECK(errs("[options.r]\ntype = \"float\"\ndefault = 1.0\nrestart = \"yes\"\n").find("options.r.restart': expected a bool") != std::string::npos);
    // a 40-character label fits, and the three fields come through
    std::vector<std::string> e2;
    auto ok = parse_config_decl("[options.r]\ntype = \"float\"\ndefault = 1.0\nlabel = \"" + std::string(40, 'x') +
                                    "\"\ngroup = \"Ring\"\nrestart = true\n", "config.toml", e2);
    REQUIRE(ok);
    CHECK(e2.empty());
    CHECK((*ok)[0].label == std::string(40, 'x'));
    CHECK((*ok)[0].group == "Ring");
    CHECK((*ok)[0].restart);
    e2.clear();
    auto quiet = parse_config_decl("[options.r]\ntype = \"string\"\ndefault = \"fire\"\n", "config.toml", e2);
    REQUIRE(quiet);
    CHECK((*quiet)[0].label.empty());
    CHECK((*quiet)[0].group.empty());
    CHECK(!(*quiet)[0].restart);            // the defaults
    // an integer is a valid float
    std::vector<std::string> e3;
    auto oknum = parse_config_decl("[options.r]\ntype = \"float\"\ndefault = 2\nmin = 0\n", "config.toml", e3);
    REQUIRE(oknum);
    CHECK(e3.empty());
    CHECK(std::get<double>((*oknum)[0].def) == 2.0);
}

TEST_CASE("mod-config: mod without options") {
    // covers: mod-config/Mod without options
    FxMod f("cfg_plain", "cfg_plain");
    std::string dir = temp_dir("plain");
    REQUIRE(f.attach(dir));
    CHECK(f.m.config == nullptr);
    CHECK(fs::is_empty(P(dir)));
    OptValue v;
    CHECK(config_get(&f.m, "radius", std::nullopt, &v, nullptr) == KY_E_NOT_FOUND);
}

TEST_CASE("mod-config: options from a zip package") {
    std::string dir = temp_dir("zip");
    std::string zip = dir + "/cfg_demo.zip";
    for (const char* name : {"mod.toml", "config.toml"}) {
        std::string t = text_of(kFx + "/cfg_demo/" + name);
        REQUIRE(mz_zip_add_mem_to_archive_file_in_place(zip.c_str(), name, t.data(), t.size(), nullptr, 0, MZ_DEFAULT_LEVEL));
    }
    FxMod f("", "cfg_demo", zip);
    REQUIRE(f.m.pkg);
    REQUIRE(f.attach(dir));
    CHECK(f.radius() == 8.0);
    CHECK(file_exists(dir + "/cfg_demo.toml"));
}

// ------------------------------------------------------------------ the player's file
TEST_CASE("mod-config: first run writes every default under its description") {
    // covers: mod-config/First run
    FxMod f("cfg_demo", "cfg_demo");
    std::string dir = temp_dir("firstrun");
    REQUIRE(f.attach(dir));
    std::string t = text_of(dir + "/cfg_demo.toml");
    size_t desc = t.find("# Blast radius in blocks.\n# float, 1.0 to 32.0, default 8.0\nradius = 8.0\n");
    size_t mode = t.find("# What the blast is made of.\n# string, one of \"fire\", \"ice\", default \"fire\"\nmode = \"fire\"\n");
    CHECK(desc != std::string::npos);
    CHECK(mode != std::string::npos);
    CHECK(desc < mode);
    CHECK(f.m.config->display == "kyubu/config/cfg_demo.toml");
}

TEST_CASE("mod-config: the player's value is read before init") {
    // covers: mod-config/Player's value
    std::string dir = temp_dir("player");
    fs::copy_file(P(kFx + "/player/changed.toml"), P(dir + "/cfg_demo.toml"));
    FxMod f("cfg_demo", "cfg_demo");
    REQUIRE(f.attach(dir));
    CHECK(f.radius() == 12.0);
    CHECK(f.m.config->from_file["radius"]);
    OptValue mode;
    REQUIRE(config_get(&f.m, "mode", OptType::String, &mode, nullptr) == KY_OK);
    CHECK(std::get<std::string>(mode) == "ice");
    CHECK(text_of(dir + "/cfg_demo.toml") == text_of(kFx + "/player/changed.toml"));   // nothing to add: untouched
}

TEST_CASE("mod-config: a mod update adds an option") {
    // covers: mod-config/Mod update adds an option
    std::string dir = temp_dir("update");
    std::string before = text_of(kFx + "/player/changed.toml");
    write_text(dir + "/cfg_demo.toml", before);
    FxMod f("cfg_demo_v2", "cfg_demo");
    REQUIRE(f.attach(dir));
    std::string after = text_of(dir + "/cfg_demo.toml");
    CHECK(after.substr(0, before.size()) == before);   // the player's lines are kept as they were
    CHECK(after.find("# Projectile speed.\n# int, 1 to 10, default 3\nspeed = 3\n") != std::string::npos);
    CHECK(f.radius() == 12.0);
    OptValue speed;
    REQUIRE(config_get(&f.m, "speed", OptType::Int, &speed, nullptr) == KY_OK);
    CHECK(std::get<int64_t>(speed) == 3);
}

TEST_CASE("mod-config: an option removed by an update stays and is reported once") {
    // covers: mod-config/Option removed by an update
    std::string dir = temp_dir("removed");
    fs::copy_file(P(kFx + "/player/extra_key.toml"), P(dir + "/cfg_demo.toml"));
    uint64_t seq = Log::get().sequence();
    FxMod f("cfg_demo", "cfg_demo");
    REQUIRE(f.attach(dir));
    config_reload(&f.m);
    std::string log = log_since(seq);
    std::string warning = "kyubu/config/cfg_demo.toml: key 'old_option' is not an option of cfg_demo";
    size_t first = log.find(warning);
    CHECK(first != std::string::npos);
    CHECK(log.find(warning, first + 1) == std::string::npos);
    CHECK(text_of(dir + "/cfg_demo.toml").find("old_option = 1") != std::string::npos);
}

TEST_CASE("mod-config: a value out of range falls back to the default") {
    // covers: mod-config/Out of range
    std::string dir = temp_dir("range");
    fs::copy_file(P(kFx + "/player/out_of_range.toml"), P(dir + "/cfg_demo.toml"));
    uint64_t seq = Log::get().sequence();
    FxMod f("cfg_demo", "cfg_demo");
    REQUIRE(f.attach(dir));
    CHECK(f.radius() == 8.0);
    CHECK_FALSE(f.m.config->from_file["radius"]);
    INFO(log_since(seq));
    CHECK(log_since(seq).find("kyubu/config/cfg_demo.toml: radius = 100.0 is not allowed (a number from 1.0 to 32.0); "
                              "using the default 8.0") != std::string::npos);
    CHECK(text_of(dir + "/cfg_demo.toml") == text_of(kFx + "/player/out_of_range.toml"));
}

TEST_CASE("mod-config: wrong types and choices in the player's file") {
    std::string dir = temp_dir("types");
    write_text(dir + "/cfg_demo.toml", "radius = \"big\"\nmode = \"water\"\n");
    uint64_t seq = Log::get().sequence();
    FxMod f("cfg_demo", "cfg_demo");
    REQUIRE(f.attach(dir));
    std::string log = log_since(seq);
    CHECK(log.find("radius = \"big\" is not allowed (a number from 1.0 to 32.0)") != std::string::npos);
    CHECK(log.find("mode = \"water\" is not allowed (one of \"fire\", \"ice\"); using the default \"fire\"") != std::string::npos);
    CHECK(f.radius() == 8.0);
}

TEST_CASE("mod-config: a broken file keeps every default and is not rewritten") {
    // covers: mod-config/Broken file
    std::string dir = temp_dir("broken");
    fs::copy_file(P(kFx + "/player/broken.toml"), P(dir + "/cfg_demo.toml"));
    uint64_t seq = Log::get().sequence();
    FxMod f("cfg_demo_v2", "cfg_demo");   // declares speed, which the file lacks: still nothing is appended
    REQUIRE(f.attach(dir));
    CHECK(f.m.config->file_broken);
    CHECK(log_since(seq).find("kyubu/config/cfg_demo.toml:3:") != std::string::npos);
    CHECK(f.radius() == 8.0);
    CHECK(text_of(dir + "/cfg_demo.toml") == text_of(kFx + "/player/broken.toml"));
    std::string err;
    CHECK(config_set(&f.m, "radius", 9.0, &err) == KY_E_UNSAFE);   // no edits to a file that does not parse
    CHECK(text_of(dir + "/cfg_demo.toml") == text_of(kFx + "/player/broken.toml"));
}

// ------------------------------------------------------------------ editing one key
TEST_CASE("mod-config: rewriting one key keeps the rest of the file") {
    CHECK(rewrite_key("# c\nradius = 8.0   # mine\nmode = \"a # b\"\n", "radius", "16.0") ==
          "# c\nradius = 16.0   # mine\nmode = \"a # b\"\n");
    CHECK(rewrite_key("# c\nradius = 8.0   # mine\nmode = \"a # b\"\n", "mode", "\"ice\"") ==
          "# c\nradius = 8.0   # mine\nmode = \"ice\"\n");
    CHECK(rewrite_key("a = 1\r\nb = 2\r\n", "b", "3") == "a = 1\r\nb = 3\r\n");
    CHECK(rewrite_key("a = 1\r\n", "b", "3") == "a = 1\r\nb = 3\r\n");
    CHECK(rewrite_key("a = 1", "b", "3") == "a = 1\nb = 3\n");
    CHECK(rewrite_key("a = 1\n[t]\nb = 2\n", "b", "3") == "a = 1\nb = 3\n\n[t]\nb = 2\n");   // top level only
    CHECK(rewrite_key("s = \"\"\"\nb = 1\n\"\"\"\nb = 2\n", "b", "3") == "s = \"\"\"\nb = 1\n\"\"\"\nb = 3\n");
    CHECK(rewrite_key("\"b\" = 2\n", "b", "3") == "\"b\" = 3\n");
}

TEST_CASE("mod-config: values are written the way a player would") {
    CHECK(format_value(100.0) == "100.0");
    CHECK(format_value(8.0) == "8.0");
    CHECK(format_value(0.1) == "0.1");
    CHECK(format_value(1234567.5) == "1234567.5");
    CHECK(format_value(1e20) == "1e+20");
    CHECK(format_value(-0.25) == "-0.25");
    CHECK(format_value(int64_t(-3)) == "-3");
    CHECK(format_value(true) == "true");
    CHECK(format_value(std::string("a\"b\\c\n")) == "\"a\\\"b\\\\c\\n\"");
}

TEST_CASE("mod-config: set and restart") {
    // covers: mod-config/Set and restart
    std::string dir = temp_dir("set");
    FxMod f("cfg_demo", "cfg_demo");
    REQUIRE(f.attach(dir));
    std::string before = text_of(dir + "/cfg_demo.toml");
    const KyubuApi* api = api_table();
    KyubuModHandle h = f.m.handle(KY_SIDE_CLIENT);
    CHECK(api->config_set(h, "radius", "16") == KY_OK);
    double r = 0;
    CHECK(api->config_get_float(h, "radius", &r) == KY_OK);
    CHECK(r == 16.0);
    std::string after = text_of(dir + "/cfg_demo.toml");
    std::string expect = before;
    expect.replace(expect.find("radius = 8.0"), 12, "radius = 16.0");
    CHECK(after == expect);   // comments and the other values unchanged

    FxMod restarted("cfg_demo", "cfg_demo");
    REQUIRE(restarted.attach(dir));
    CHECK(restarted.radius() == 16.0);
    CHECK(text_of(dir + "/cfg_demo.toml") == expect);
}

TEST_CASE("mod-config: invalid set") {
    // covers: mod-config/Invalid set
    std::string dir = temp_dir("invalid");
    FxMod f("cfg_demo", "cfg_demo");
    REQUIRE(f.attach(dir));
    std::string before = text_of(dir + "/cfg_demo.toml");
    const KyubuApi* api = api_table();
    KyubuModHandle h = f.m.handle(KY_SIDE_CLIENT);
    CHECK(api->config_set(h, "radius", "100") == KY_E_INVALID_ARG);
    CHECK(api->config_set(h, "radius", "far") == KY_E_INVALID_ARG);
    CHECK(api->config_set(h, "mode", "water") == KY_E_INVALID_ARG);
    CHECK(api->config_set(h, "nope", "1") == KY_E_NOT_FOUND);
    CHECK(f.radius() == 8.0);
    CHECK(text_of(dir + "/cfg_demo.toml") == before);
}

TEST_CASE("mod-config: reads check the key and the type") {
    std::string dir = temp_dir("reads");
    FxMod f("cfg_demo_v2", "cfg_demo");
    REQUIRE(f.attach(dir));
    const KyubuApi* api = api_table();
    KyubuModHandle h = f.m.handle(KY_SIDE_SERVER);
    int b = 0;
    int64_t i = 0;
    double d = 0;
    char s[8] = {}, tiny[3] = {};
    CHECK(api->config_get_int(h, "speed", &i) == KY_OK);
    CHECK(i == 3);
    CHECK(api->config_get_string(h, "mode", s, sizeof s) == KY_OK);
    CHECK(std::string(s) == "fire");
    CHECK(api->config_get_string(h, "mode", tiny, sizeof tiny) == KY_E_BUFFER);
    CHECK(api->config_get_bool(h, "radius", &b) == KY_E_INVALID_ARG);     // a float, read as a bool
    CHECK(api->config_get_float(h, "speed", &d) == KY_E_INVALID_ARG);     // an int, read as a float
    CHECK(api->config_get_float(h, "missing", &d) == KY_E_NOT_FOUND);
    CHECK(api->config_get_float(nullptr, "radius", &d) == KY_E_INVALID_ARG);
    CHECK(api->config_set(h, "speed", "4.5") == KY_E_INVALID_ARG);
    CHECK(api->config_set(h, "speed", "4") == KY_OK);
    CHECK(api->config_set(h, "mode", "ice") == KY_OK);
    CHECK(api->config_get_string(h, "mode", s, sizeof s) == KY_OK);
    CHECK(std::string(s) == "ice");
}

TEST_CASE("mod-config: a 1.0 mod still loads (API size check)") {
    const KyubuApi* api = api_table();
    CHECK(api->version == ((1u << 16) | 4u));   // 1.2 starts with the error code KY_E_DENIED; the management members are appended next (add-mod-manager); 1.3 appends creature_set_flags, 1.4 the creature-body members (add-dragon-boss)
    CHECK(api->size == sizeof(KyubuApi));
    // appended after the game API's members (creature_give_item is its last), so every 1.0 offset is unchanged
    CHECK(offsetof(KyubuApi, config_get_bool) == offsetof(KyubuApi, creature_give_item) + sizeof(void*));
    // the ability calls (add-ability-api) are appended after the options: no earlier offset moves
    CHECK(offsetof(KyubuApi, ability_define) == offsetof(KyubuApi, config_set) + sizeof(void*));
    CHECK(offsetof(KyubuApi, ability_number) == offsetof(KyubuApi, ability_define) + sizeof(void*));
    CHECK(offsetof(KyubuApi, ability_slot_owner) + sizeof(void*) == offsetof(KyubuApi, ability_get));
    // the ui calls follow ability_animation_bind, then the particle call (add-particle-effects); the light calls (add-light-api)
    // end the table
    CHECK(offsetof(KyubuApi, ability_animation_bind) + sizeof(void*) == offsetof(KyubuApi, ui_node_find));
    CHECK(offsetof(KyubuApi, render_get) + sizeof(void*) == offsetof(KyubuApi, particle_spawn));
    CHECK(offsetof(KyubuApi, particle_spawn) + sizeof(void*) == offsetof(KyubuApi, light_spawn));
    // mod management (1.2) is appended after the lights, the last section of the table for now (with game_restart)
    CHECK(offsetof(KyubuApi, light_remove) + sizeof(void*) == offsetof(KyubuApi, mods_count));
    CHECK(offsetof(KyubuApi, mods_package_hash) + sizeof(void*) == offsetof(KyubuApi, mods_option_count));
    CHECK(offsetof(KyubuApi, mods_rescan) + sizeof(void*) == offsetof(KyubuApi, mods_queue));
    CHECK(offsetof(KyubuApi, mods_queue_get) + sizeof(void*) == offsetof(KyubuApi, game_restart));   // game_restart (D3) ends the table
    CHECK(offsetof(KyubuApi, game_restart) + sizeof(void*) == offsetof(KyubuApi, creature_set_flags));   // creature_set_flags (1.3) ends the table
    CHECK(offsetof(KyubuApi, creature_set_flags) + sizeof(void*) == offsetof(KyubuApi, body_define)); CHECK(offsetof(KyubuApi, creature_body_point) + sizeof(void*) == offsetof(KyubuApi, creature_set_yaw)); CHECK(offsetof(KyubuApi, creature_set_yaw) + sizeof(void*) == offsetof(KyubuApi, ability_animation_bind_for)); CHECK(offsetof(KyubuApi, ability_animation_bind_for) + sizeof(void*) == offsetof(KyubuApi, ability_cooldown_set)); CHECK(offsetof(KyubuApi, ability_cooldown_set) + sizeof(void*) == offsetof(KyubuApi, world_block)); CHECK(offsetof(KyubuApi, world_block) + sizeof(void*) == offsetof(KyubuApi, creature_hit)); CHECK(offsetof(KyubuApi, creature_hit) + sizeof(void*) == offsetof(KyubuApi, creature_state_get)); CHECK(offsetof(KyubuApi, creature_state_get) + sizeof(void*) == offsetof(KyubuApi, creature_state_set)); CHECK(offsetof(KyubuApi, creature_state_set) + sizeof(void*) == offsetof(KyubuApi, shot_take)); CHECK(offsetof(KyubuApi, shot_take) + sizeof(void*) == offsetof(KyubuApi, projectile_push)); CHECK(offsetof(KyubuApi, projectile_push) + sizeof(void*) == sizeof(KyubuApi));
    CHECK(offsetof(KyubuApi, animation_preview_clear) + sizeof(void*) == offsetof(KyubuApi, ability_animation_bind));
    CHECK(offsetof(KyubuApi, mod_state) + sizeof(void*) == offsetof(KyubuApi, game_available));
    CHECK(api_version_supported((1u << 16) | 0u));
    CHECK(api_version_supported((1u << 16) | 1u));
    CHECK(api_version_supported((1u << 16) | 2u));
    CHECK(api_version_supported((1u << 16) | 3u));
    CHECK(api_version_supported((1u << 16) | 4u));
    CHECK_FALSE(api_version_supported((1u << 16) | 5u));
    CHECK_FALSE(api_version_supported(2u << 16));
}

// ------------------------------------------------------------------ change notification
TEST_CASE("mod-config: reload after editing the file emits config.changed") {
    // covers: mod-config/Reload after editing the file
    Loader& l = L();
    auto saved_phase = l.phase.load();
    auto saved_host = l.host.host;
    auto saved_mode = l.mode;
    l.host.host = KY_HOST_CLIENT;
    l.mode = KY_MODE_MENU_OR_SINGLEPLAYER;
    l.phase = KY_PHASE_GAMEREADY;

    std::string dir = temp_dir("reload");
    FxMod f("cfg_demo", "cfg_demo");
    REQUIRE(f.attach(dir));
    std::vector<std::string> seen;
    KyubuId sub[2];
    int i = 0;
    for (KyubuSide s : {KY_SIDE_CLIENT, KY_SIDE_SERVER}) {
        sub[i++] = Events::get().subscribe_internal("config.changed", s, 0, [&, s](KyubuEvent* ev) {
            auto d = (KyubuConfigChangedData*)ev->data;
            OptValue v;
            config_get(&f.m, d->key, std::nullopt, &v, nullptr);
            seen.push_back(std::string(s == KY_SIDE_CLIENT ? "client " : "server ") + d->mod + "." + d->key + "=" + format_value(v));
        });
    }
    std::string t = text_of(dir + "/cfg_demo.toml");
    t.replace(t.find("radius = 8.0"), 12, "radius = 20.0");
    write_text(dir + "/cfg_demo.toml", t);
    config_pump();
    CHECK(seen.empty());                        // nothing until the file is reloaded
    auto lines = config_reload(&f.m);
    CHECK(lines.back().find("1 option(s) changed") != std::string::npos);
    config_pump();
    INFO(joined(seen));
    CHECK(seen == std::vector<std::string>{"client cfg_demo.radius=20.0", "server cfg_demo.radius=20.0"});

    seen.clear();
    config_reload(&f.m);                        // the same value again: no event
    CHECK(config_set(&f.m, "radius", 20.0, nullptr) == KY_OK);
    config_pump();
    CHECK(seen.empty());

    CHECK(config_set(&f.m, "radius", int64_t(24), nullptr) == KY_OK);   // an integer is a valid float
    CHECK(config_set(&f.m, "radius", 25.0, nullptr) == KY_OK);
    config_pump();                              // one event per option and pump
    INFO(joined(seen));
    CHECK(seen == std::vector<std::string>{"client cfg_demo.radius=25.0", "server cfg_demo.radius=25.0"});

    for (KyubuId id : sub) Events::get().unsubscribe(nullptr, id);
    l.phase = saved_phase;
    l.host.host = saved_host;
    l.mode = saved_mode;
}

// ------------------------------------------------------------------ mod loading
TEST_CASE("mod-config: mod loading reads options before any entry point, and only for enabled mods") {
    // covers: mod-config/Default of the wrong type (through mod loading), mod-config/Restricted script,
    // and "Disabled mods' files SHALL NOT be created or changed"
    Loader& l = L();
    Loader saved_dirs;
    saved_dirs.kyubu_dir = l.kyubu_dir;
    saved_dirs.mods_dir = l.mods_dir;
    auto saved_host = l.host.host;
    auto saved_mode = l.mode;
    auto saved_enabled = l.config.mod_enabled;
    auto saved_phase = l.phase.load();

    std::string game = temp_dir("loading");
    l.kyubu_dir = game + "/kyubu";
    l.mods_dir = game + "/mods";
    l.host.host = KY_HOST_CLIENT;
    l.mode = KY_MODE_MENU_OR_SINGLEPLAYER;
    l.phase = KY_PHASE_PREINIT;
    l.config.mod_enabled["cfg_disabled"] = false;
    fs::create_directories(P(l.mods_dir));
    for (const char* m : {"cfg_demo", "cfg_bad_default", "cfg_disabled", "cfg_lua", "cfg_plain"})
        fs::copy(P(kFx + "/" + m), P(l.mods_dir + "/" + m), fs::copy_options::recursive);

    uint64_t seq = Log::get().sequence();
    auto& mm = ModManager::get();
    mm.discover_and_resolve();
    mm.load_native();
    LuaRuntime::get().load_all();
    std::string log = log_since(seq);
    std::string cfg = l.kyubu_dir + "/config/";

    Mod* bad = mm.find("cfg_bad_default");
    REQUIRE(bad);
    CHECK(bad->state == ModState::Failed);
    CHECK(bad->reason.find("options.radius.default") != std::string::npos);
    CHECK_FALSE(file_exists(cfg + "cfg_bad_default.toml"));

    Mod* off = mm.find("cfg_disabled");
    REQUIRE(off);
    CHECK(off->state == ModState::Disabled);
    CHECK(off->config == nullptr);
    CHECK_FALSE(file_exists(cfg + "cfg_disabled.toml"));

    CHECK(mm.find("cfg_demo")->state == ModState::Loaded);
    CHECK(file_exists(cfg + "cfg_demo.toml"));
    CHECK(mm.find("cfg_plain")->state == ModState::Loaded);
    CHECK_FALSE(file_exists(cfg + "cfg_plain.toml"));

    // the restricted script read and set its own option, and reached nothing else
    Mod* lua = mm.find("cfg_lua");
    REQUIRE(lua);
    CHECK(lua->state == ModState::Loaded);
    CHECK(log.find("cfg_lua: get=0.5 set=true after=0.25 bad=nil/volume = 5.0 is not allowed (a number from 0.0 to 1.0) "
                   "fns=2 other=cfg_lua has no option 'radius'") != std::string::npos);
    CHECK(text_of(cfg + "cfg_lua.toml").find("volume = 0.25\n") != std::string::npos);

    // the console command (covers: mod-config/Set from the console, at unit level; the in-game check is task 3.1)
    register_builtin_commands();
    std::string out;
    auto run = [&](const std::string& line) {
        out.clear();
        Console::get().execute(line, [&](const std::string& s) { out += s + "\n"; });
        return out;
    };
    CHECK(run("config").find("cfg_demo") != std::string::npos);
    CHECK(out.find("cfg_disabled") == std::string::npos);
    CHECK(out.find("cfg_plain") == std::string::npos);
    CHECK(run("config cfg_demo radius 16") == "cfg_demo: radius = 16.0 (saved to kyubu/config/cfg_demo.toml)\n");
    CHECK(run("config cfg_demo").find("  radius = 16.0  (default 8.0, from the file)\n") != std::string::npos);
    CHECK(out.find("  mode = \"fire\"  (default \"fire\", from the file)\n") != std::string::npos);
    CHECK(text_of(cfg + "cfg_demo.toml").find("\nradius = 16.0\n") != std::string::npos);
    CHECK(run("config cfg_demo mode \"ice\"").find("mode = \"ice\"") != std::string::npos);
    CHECK(run("config cfg_demo radius 100").find("radius = 100 is not allowed (a number from 1.0 to 32.0)") != std::string::npos);
    CHECK(run("config cfg_plain").find("no mod 'cfg_plain' with options") != std::string::npos);
    std::string t = text_of(cfg + "cfg_demo.toml");
    t.replace(t.find("radius = 16.0"), 13, "radius = 4.0");
    write_text(cfg + "cfg_demo.toml", t);
    CHECK(run("config reload cfg_demo").find("  radius = 4.0 (was 16.0)\ncfg_demo: reloaded kyubu/config/cfg_demo.toml, 1 option(s) changed\n") != std::string::npos);

    // leave no mods behind for other tests
    config_pump();
    l.mods_dir = temp_dir("loading-empty");
    l.config.mod_enabled = saved_enabled;
    mm.discover_and_resolve();
    l.kyubu_dir = saved_dirs.kyubu_dir;
    l.mods_dir = saved_dirs.mods_dir;
    l.host.host = saved_host;
    l.mode = saved_mode;
    l.phase = saved_phase;
}
