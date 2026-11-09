// Unit tests of the animation binding (openspec add-ability-animation task 3.4): ability_animation_bind, kyubu.animation.bind and the
// `anim` key of kyubu.ability.define, through the real API table with mods that are not loaded from disk, and a Lua mod pair.
#include "doctest.h"

#include <cstring>
#include <filesystem>
#include <new>
#include <string>

#include "abilities.h"
#include "animations.h"
#include "api.h"
#include "call.h"
#include "console.h"
#include "game.h"
#include "loader.h"
#include "log.h"
#include "luart.h"
#include "modmanager.h"
#include "registry.h"

using namespace kyubu;

namespace {

Registries& reset_registries() {
    Registries& g = Registries::get();
    g.~Registries();
    new (&g) Registries();
    g.init_builtin();
    ability_callbacks_clear();
    return g;
}

struct BindMod {
    Mod mod;
    explicit BindMod(const char* id, const char* trust = "full") {
        mod.id = id;
        mod.state = ModState::Loaded;
        mod.manifest.sides = KY_SIDE_BOTH;
        mod.manifest.trust = trust;
        for (int s = 1; s <= 2; ++s) { mod.ctx[s].mod = &mod; mod.ctx[s].side = (KyubuSide)s; }
    }
    KyubuModHandle on(KyubuSide s) { return mod.handle(s); }
};

// The register phase on the client, the pose provider available, and the binding table empty at both ends.
struct BindState {
    KyubuPhase phase = L().phase.load();
    decltype(L().host.host) host = L().host.host;
    decltype(L().mode) mode = L().mode;
    BindState() {
        L().host.host = KY_HOST_CLIENT;
        L().mode = KY_MODE_MENU_OR_SINGLEPLAYER;
        L().phase = KY_PHASE_REGISTER;
        game::force_primitive_for_test(game::Prim::animation_pose, 1);
        Animations::get().clear_bindings();
    }
    ~BindState() {
        game::force_primitive_for_test(game::Prim::animation_pose, -1);
        Animations::get().clear_bindings();
        L().phase = phase;
        L().host.host = host;
        L().mode = mode;
    }
};

std::string clip_text(int duration_ms) {
    return "duration = " + std::to_string(duration_ms) +
           "\n\n[[track]]\npart = \"hand_r\"\nchannel = \"rot\"\nkeys = [ { t = 0, v = [0, 0, 0] }, { t = 1, v = [30, 0, 0] } ]\n";
}

void make_clip(const char* mod, const char* name, int ms = 400) {
    std::string errors;
    REQUIRE_MESSAGE(Animations::get().define_from_text(mod, name, clip_text(ms), &errors), errors);
}

bool define_ability(BindMod& m, const char* id, KyubuSide side = KY_SIDE_CLIENT) {
    KyubuAbilityDef d = {};
    d.size = sizeof d;
    d.duration_ms = 800;
    d.max_rank = 1;
    static const uint32_t cooldown[2] = {1000, 1000};
    d.cooldown_ms = cooldown;
    return api_table()->ability_define(m.on(side), id, &d) == KY_OK;
}

std::string bound(const char* ability) {
    std::string clip;
    return Animations::get().bound_clip(ability, &clip) ? clip : "<none>";
}

bool logged(uint64_t since, const std::string& part) {
    for (auto& e : Log::get().since(since))
        if (e.text.find(part) != std::string::npos) return true;
    return false;
}

}  // namespace

TEST_CASE("ability-registry: a clip is bound to the caller's own ability, and bound again it is replaced") {
    // covers: ability-registry/Binding an animation clip to an ability: Bound by the definition (the native half)
    // covers: ability-registry/Rebinding and missing clips: Rebinding
    // covers: ability-registry/No clip
    BindState state;
    reset_registries();
    BindMod demo("demo");
    make_clip("demo", "slam");
    make_clip("demo", "slam2");
    REQUIRE(define_ability(demo, "demo:slam"));
    CHECK(bound("demo:slam") == "<none>");   // no clip: nothing recorded, so the ability looks as it did

    const KyubuApi* api = api_table();
    CHECK(api->ability_animation_bind(demo.on(KY_SIDE_CLIENT), "demo:slam", "demo:slam") == KY_OK);
    CHECK(bound("demo:slam") == "demo:slam");
    // a second clip replaces the first: one clip per ability
    CHECK(api->ability_animation_bind(demo.on(KY_SIDE_CLIENT), "demo:slam", "demo:slam2") == KY_OK);
    CHECK(bound("demo:slam") == "demo:slam2");
    // the same again is fine
    CHECK(api->ability_animation_bind(demo.on(KY_SIDE_CLIENT), "demo:slam", "demo:slam2") == KY_OK);
    // after the freeze too: the editor rebinds while the game runs
    Registries::get().freeze([](const std::string&) { return 0; });
    CHECK(api->ability_animation_bind(demo.on(KY_SIDE_CLIENT), "demo:slam", "demo:slam") == KY_OK);
    CHECK(bound("demo:slam") == "demo:slam");
}

TEST_CASE("ability-registry: a clip bound for one creature wins for it alone, and is dropped") {
    // covers: add-skills-unleashed-multiplayer 8.1 (ability_animation_bind_for)
    BindState state;
    reset_registries();
    BindMod demo("demo");
    make_clip("demo", "charge");
    make_clip("demo", "release");
    REQUIRE(define_ability(demo, "demo:fire"));
    const KyubuApi* api = api_table();
    REQUIRE(api->ability_animation_bind(demo.on(KY_SIDE_CLIENT), "demo:fire", "demo:charge") == KY_OK);
    CHECK(api->ability_animation_bind_for(demo.on(KY_SIDE_CLIENT), "demo:fire", 42, "demo:release") == KY_OK);
    std::string clip;
    REQUIRE(Animations::get().bound_clip("demo:fire", 42, &clip));
    CHECK(clip == "demo:release");
    REQUIRE(Animations::get().bound_clip("demo:fire", 43, &clip));
    CHECK(clip == "demo:charge");   // another creature keeps the ability's clip
    CHECK(bound("demo:fire") == "demo:charge");
    // the refusals are the ability binding's, and creature id 0 names nobody
    CHECK(api->ability_animation_bind_for(demo.on(KY_SIDE_CLIENT), "demo:fire", 0, "demo:release") == KY_E_INVALID_ARG);
    CHECK(api->ability_animation_bind_for(demo.on(KY_SIDE_CLIENT), "demo:fire", 42, "demo:nope") == KY_E_NOT_FOUND);
    CHECK(api->ability_animation_bind_for(demo.on(KY_SIDE_CLIENT), "demo:none", 42, "demo:release") == KY_E_NOT_FOUND);
    CHECK(api->ability_animation_bind_for(demo.on(KY_SIDE_SERVER), "demo:fire", 42, "demo:release") == KY_OK);   // either side
    // NULL drops it
    CHECK(api->ability_animation_bind_for(demo.on(KY_SIDE_CLIENT), "demo:fire", 42, nullptr) == KY_OK);
    REQUIRE(Animations::get().bound_clip("demo:fire", 42, &clip));
    CHECK(clip == "demo:charge");
    // a destroyed creature's binding and a left world's go
    Animations::get().bind_for("demo:fire", 42, "demo:release");
    Animations::get().bind_for("demo:fire", 50, "demo:release");
    Animations::get().drop_creature(42);
    REQUIRE(Animations::get().bound_clip("demo:fire", 42, &clip));
    CHECK(clip == "demo:charge");
    REQUIRE(Animations::get().bound_clip("demo:fire", 50, &clip));
    CHECK(clip == "demo:release");
    Animations::get().clear_creature_bindings();
    REQUIRE(Animations::get().bound_clip("demo:fire", 50, &clip));
    CHECK(clip == "demo:charge");
}

TEST_CASE("ability-registry: another mod's ability and a vanilla one are refused") {
    // covers: ability-registry/Which abilities take a clip: Another mod's ability; Vanilla refused
    BindState state;
    reset_registries();
    BindMod demo("demo"), other("other");
    make_clip("demo", "slam");
    REQUIRE(define_ability(demo, "demo:slam"));
    const KyubuApi* api = api_table();
    const uint64_t seq = Log::get().sequence();

    CHECK(api->ability_animation_bind(other.on(KY_SIDE_CLIENT), "demo:slam", "demo:slam") == KY_E_NAMESPACE);
    CHECK(bound("demo:slam") == "<none>");
    CHECK(logged(seq, "demo:slam belongs to mod demo, not other"));
    CHECK(api->ability_animation_bind(demo.on(KY_SIDE_CLIENT), "cubeworld:smash", "demo:slam") == KY_E_INVALID_ARG);
    CHECK(bound("cubeworld:smash") == "<none>");
    CHECK(logged(seq, "cubeworld:smash is not a mod ability"));
    // the loader's own placeholder is not a mod's ability either
    CHECK(api->ability_animation_bind(demo.on(KY_SIDE_CLIENT), "kyubu:placeholder", "demo:slam") == KY_E_INVALID_ARG);
    // an ability nobody defined
    CHECK(api->ability_animation_bind(demo.on(KY_SIDE_CLIENT), "demo:nothing", "demo:slam") == KY_E_NOT_FOUND);
    // arguments
    CHECK(api->ability_animation_bind(demo.on(KY_SIDE_CLIENT), nullptr, "demo:slam") == KY_E_INVALID_ARG);
    CHECK(api->ability_animation_bind(demo.on(KY_SIDE_CLIENT), "demo:slam", nullptr) == KY_E_INVALID_ARG);
    CHECK(api->ability_animation_bind(nullptr, "demo:slam", "demo:slam") == KY_E_INVALID_ARG);
}

TEST_CASE("ability-registry: a clip that does not exist is an error naming it, and keeps the old binding") {
    // covers: ability-registry/Rebinding and missing clips: A clip that does not exist
    BindState state;
    reset_registries();
    BindMod demo("demo");
    make_clip("demo", "slam");
    REQUIRE(define_ability(demo, "demo:slam"));
    const KyubuApi* api = api_table();
    REQUIRE(api->ability_animation_bind(demo.on(KY_SIDE_CLIENT), "demo:slam", "demo:slam") == KY_OK);
    const uint64_t seq = Log::get().sequence();
    CHECK(api->ability_animation_bind(demo.on(KY_SIDE_CLIENT), "demo:slam", "demo:missing") == KY_E_NOT_FOUND);
    CHECK(logged(seq, "no clip demo:missing"));
    CHECK(bound("demo:slam") == "demo:slam");
}

TEST_CASE("game-api: every trust level may bind, on either side, and a server side records the binding") {
    // covers: game-api/Animation binding call: Server side
    BindState state;
    reset_registries();
    BindMod restricted("rmod", "restricted");
    make_clip("rmod", "c");
    // a restricted mod has no ability_define, but the call has no trust rule: a missing ability is the only refusal
    CHECK(api_table()->ability_animation_bind(restricted.on(KY_SIDE_CLIENT), "rmod:a", "rmod:c") == KY_E_NOT_FOUND);
    // an ability that is defined (here by a full mod's call in the same namespace) binds from either side
    BindMod full("fmod");
    make_clip("fmod", "c");
    REQUIRE(define_ability(full, "fmod:a"));
    REQUIRE(define_ability(full, "fmod:b", KY_SIDE_SERVER));
    CHECK(api_table()->ability_animation_bind(full.on(KY_SIDE_SERVER), "fmod:b", "fmod:c") == KY_OK);
    CHECK(bound("fmod:b") == "fmod:c");
    CHECK(api_table()->ability_animation_bind(full.on(KY_SIDE_CLIENT), "fmod:a", "fmod:c") == KY_OK);
    CHECK(bound("fmod:a") == "fmod:c");
    // the binding is data: the registry's clip gives the duration the server side needs
    const game::anim::ClipEntry* e = Animations::get().clips().set().find("fmod:c");
    REQUIRE(e);
    CHECK(e->clip.duration_ms == 400);
}

TEST_CASE("game-api: the binding is unavailable on a build whose pose provider is not verified") {
    // covers: game-api/Animation binding availability: Unverified build
    BindState state;
    reset_registries();
    BindMod demo("demo");
    make_clip("demo", "slam");
    REQUIRE(define_ability(demo, "demo:slam"));
    REQUIRE(define_ability(demo, "demo:srv", KY_SIDE_SERVER));
    game::force_primitive_for_test(game::Prim::animation_pose, 0);
    const KyubuApi* api = api_table();
    const uint64_t seq = Log::get().sequence();
    CHECK(api->ability_animation_bind(demo.on(KY_SIDE_CLIENT), "demo:slam", "demo:slam") == KY_E_UNAVAILABLE);
    CHECK(bound("demo:slam") == "<none>");   // no look changes
    CHECK(logged(seq, "the pose provider is unavailable on this build"));
    // a server side draws nothing: it still records
    CHECK(api->ability_animation_bind(demo.on(KY_SIDE_SERVER), "demo:srv", "demo:slam") == KY_OK);
    CHECK(bound("demo:srv") == "demo:slam");
    // and the call works again once it is verified
    game::force_primitive_for_test(game::Prim::animation_pose, 1);
    CHECK(api->ability_animation_bind(demo.on(KY_SIDE_CLIENT), "demo:slam", "demo:slam") == KY_OK);
}

TEST_CASE("ability-registry: the Lua anim key and kyubu.animation.bind") {
    // covers: ability-registry/Binding an animation clip to an ability: Bound by the definition (the Lua half)
    // covers: ability-registry/Which abilities take a clip: Another mod's ability (the Lua half, a restricted mod)
    // covers: ability-registry/Rebinding and missing clips: A clip that does not exist (the Lua half)
    BindState state;
    Registries& R = reset_registries();
    Loader& l = L();
    const std::string saved_kyubu = l.kyubu_dir, saved_mods = l.mods_dir;
    make_clip("fx_anim_a", "clip");
    make_clip("fx_anim_a", "clip2");

    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    auto temp = [&](const char* name) {
        std::filesystem::path p = std::filesystem::path(tmp) / (std::string("kyubu-anim-bind-") + name + "-" + std::to_string(GetCurrentProcessId()));
        std::error_code ec;
        std::filesystem::remove_all(p, ec);
        std::filesystem::create_directories(p);
        return p.string();
    };
    const std::string game_dir = temp("lua");
    l.kyubu_dir = game_dir + "/kyubu";
    l.mods_dir = game_dir + "/mods";
    auto make_mod = [&](const char* id, const char* trust, const char* script) {
        const std::string dir = l.mods_dir + "/" + id;
        REQUIRE(make_dirs(dir + "/scripts"));
        REQUIRE(write_text(dir + "/mod.toml", std::string("id = \"") + id + "\"\nname = \"Binding: " + id + "\"\nversion = \"1.0.0\"\n"
                           "kyubu = \"^1.0\"\nsides = [\"client\"]\ntrust = \"" + trust + "\"\ndescription = \"Unit test: kyubu.animation.\"\n\n"
                           "[scripts]\nclient = \"scripts/client.lua\"\n"));
        REQUIRE(write_text(dir + "/scripts/client.lua", script));
    };
    make_mod("fx_anim_a", "full", R"(
local r = {}
local function note(name, ok, err) r[#r + 1] = name .. "=" .. tostring(ok) .. (err and ("/" .. tostring(err)) or "") end
note("with_anim", kyubu.ability.define("fx_anim_a:burst", { duration_ms = 800, cooldown_ms = 5000, anim = "fx_anim_a:clip" }))
note("no_anim", kyubu.ability.define("fx_anim_a:plain", { duration_ms = 800, cooldown_ms = 5000 }))
note("missing_clip", kyubu.ability.define("fx_anim_a:lost", { duration_ms = 800, cooldown_ms = 5000, anim = "fx_anim_a:missing" }))
note("bad_ability", kyubu.ability.define("fx_anim_a:bad", { duration_ms = 0, anim = "fx_anim_a:clip" }))
note("rebind", kyubu.animation.bind("fx_anim_a:burst", "fx_anim_a:clip2"))
note("vanilla", kyubu.animation.bind("cubeworld:smash", "fx_anim_a:clip"))
local ok, err = pcall(kyubu.ability.define, "fx_anim_a:worse", { anim = 5 })
r[#r + 1] = "anim_number=" .. tostring(ok) .. "/" .. tostring(err):gsub("^.-:%d+: ", "")
ok, err = pcall(kyubu.animation.bind, "fx_anim_a:burst")
r[#r + 1] = "bind_one_arg=" .. tostring(ok)
kyubu.info("fx_anim: " .. table.concat(r, " | "))
)");
    make_mod("fx_anim_b", "restricted", R"(
kyubu.command("fx_anim_b:try", "unit test", function(args, print)
  print("has_ability", tostring(kyubu.ability))
  print("other", kyubu.animation.bind("fx_anim_a:plain", "fx_anim_a:clip"))
end)
)");

    const uint64_t seq = Log::get().sequence();
    auto& mm = ModManager::get();
    mm.discover_and_resolve();
    mm.load_native();
    LuaRuntime::get().load_all();
    std::string line;
    for (auto& entry : Log::get().since(seq))
        if (entry.text.find("fx_anim: ") != std::string::npos && entry.text.find("with_anim=") != std::string::npos) line = entry.text;
    Mod* a = mm.find("fx_anim_a");
    Mod* b = mm.find("fx_anim_b");
    REQUIRE(a);
    REQUIRE(b);
    CHECK(a->state == ModState::Loaded);
    CHECK(b->state == ModState::Loaded);
    CHECK_MESSAGE(!line.empty(), "the script did not log its results");
    auto has = [&](const char* part) { CHECK_MESSAGE(line.find(part) != std::string::npos, part, " in: ", line); };
    has("with_anim=true");
    has("no_anim=true");
    has("missing_clip=nil/no clip fx_anim_a:missing");   // defined, and the clip is the error
    has("bad_ability=nil/invalid argument");             // the definition's own error comes first: nothing binds
    has("rebind=true");
    has("vanilla=nil/cubeworld:smash is not a mod ability");
    has("anim_number=false/kyubu.ability.define: 'anim' must be a clip id");
    has("bind_one_arg=false");

    CHECK(bound("fx_anim_a:burst") == "fx_anim_a:clip2");   // the anim key bound clip, then bind replaced it
    CHECK(bound("fx_anim_a:plain") == "<none>");
    CHECK(bound("fx_anim_a:bad") == "<none>");
    CHECK(logged(seq, "no clip fx_anim_a:missing"));
    CHECK(bound("fx_anim_a:lost") == "<none>");
    uint32_t n = 0;
    R.freeze([](const std::string&) { return 0; });   // a mod's number exists after the freeze
    CHECK(R.lookup("kyubu:abilities", "fx_anim_a:lost", &n) == KY_OK);   // defined without a clip
    CHECK(R.lookup("kyubu:abilities", "fx_anim_a:bad", &n) == KY_E_NOT_FOUND);

    // a restricted mod has kyubu.animation but no kyubu.ability, and binds only its own
    std::string out;
    Console::get().execute("fx_anim_b:try", [&](const std::string& s) { out += s + "\n"; });
    Console::get().remove_mod(b);
    CHECK_MESSAGE(out.find("has_ability nil\n") != std::string::npos, out);
    CHECK_MESSAGE(out.find("other nil fx_anim_a:plain belongs to mod fx_anim_a, not fx_anim_b\n") != std::string::npos, out);
    CHECK(bound("fx_anim_a:plain") == "<none>");

    reset_registries();
    l.mods_dir = temp("lua-empty");
    mm.discover_and_resolve();
    l.kyubu_dir = saved_kyubu;
    l.mods_dir = saved_mods;
}
