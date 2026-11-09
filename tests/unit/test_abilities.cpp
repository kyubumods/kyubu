#include "doctest.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <new>

#include "ability_bar.h"
#include "ability_iconfile.h"
#include "ability_skilltree.h"
#include "ability_stats.h"
#include "ability_step.h"
#include "abilities.h"
#include "animations.h"
#include "api.h"
#include "config.h"
#include "console.h"
#include "game.h"
#include "handles.h"
#include "loader.h"
#include "log.h"
#include "luart.h"
#include "modmanager.h"
#include "registry.h"
#include "symbols.h"

using namespace kyubu;

namespace {

int by_name(const std::string& o) { return o == "fx_ability" ? 0 : o == "a" ? 1 : o == "b" ? 2 : 100; }

// Registries is a process-wide singleton that nothing else in the test run touches: start it again, with its built-ins.
Registries& reset_registries() {
    Registries& g = Registries::get();
    g.~Registries();
    new (&g) Registries();
    g.init_builtin();
    ability_callbacks_clear();
    return g;
}

uint32_t ability_count() {
    uint32_t n = 0;
    Registries::get().count("kyubu:abilities", &n);
    return n;
}

// A mod that is not loaded from disk: enough for the API table's checks.
struct DemoMod {
    Mod mod;
    explicit DemoMod(const char* id) {
        mod.id = id;
        mod.state = ModState::Loaded;
        mod.manifest.sides = KY_SIDE_BOTH;
        for (int s = 1; s <= 2; ++s) { mod.ctx[s].mod = &mod; mod.ctx[s].side = (KyubuSide)s; }
    }
    KyubuModHandle on(KyubuSide s) { return mod.handle(s); }
};

KyubuAbilityDef good_def() {
    static const uint32_t cooldown[3] = {6000, 5500, 5000};
    KyubuAbilityDef d = {};
    d.size = sizeof d;
    d.duration_ms = 800;
    d.mp_cost = 0.25f;
    d.max_rank = 2;
    d.cooldown_ms = cooldown;
    d.slot = KY_ABILITY_SLOT_FOURTH;
    d.class_type = 1;
    d.specialization = 1;
    return d;
}

struct Loaded {
    KyubuPhase phase = L().phase.load();
    decltype(L().host.host) host = L().host.host;
    decltype(L().mode) mode = L().mode;
    Loaded() { L().host.host = KY_HOST_CLIENT; L().mode = KY_MODE_MENU_OR_SINGLEPLAYER; L().phase = KY_PHASE_REGISTER; }
    ~Loaded() { L().phase = phase; L().host.host = host; L().mode = mode; }
};

bool logged(uint64_t since, const std::string& part) {
    for (auto& e : Log::get().since(since))
        if (e.text.find(part) != std::string::npos) return true;
    return false;
}

void KYUBU_CALL noop_callback(void*, const KyubuAbilityCtx*) {}

}  // namespace

TEST_CASE("ability-registry: defining an ability, from both sides") {
    // covers: ability-registry/A full-trust mod defines an ability (the native half)
    Loaded state;
    Registries& R = reset_registries();
    DemoMod demo("demo");
    const KyubuApi* api = api_table();
    KyubuAbilityDef d = good_def();
    d.name = "demo:burst_name";
    d.icon = "cubeworld:smash";
    int start_user = 0, other_user = 0;
    d.on_start = noop_callback;
    d.user = &start_user;
    CHECK(api->ability_define(demo.on(KY_SIDE_CLIENT), "demo:burst", &d) == KY_OK);
    CHECK(ability_count() == 113);

    // the same data from the other side is allowed, and keeps that side's callbacks
    d.user = &other_user;
    d.on_start = nullptr;
    CHECK(api->ability_define(demo.on(KY_SIDE_SERVER), "demo:burst", &d) == KY_OK);
    CHECK(ability_count() == 113);
    const AbilityCallbacks* c = ability_callbacks("demo:burst", KY_SIDE_CLIENT);
    REQUIRE(c);
    CHECK(c->user == &start_user);
    CHECK(c->on_start == noop_callback);
    const AbilityCallbacks* s = ability_callbacks("demo:burst", KY_SIDE_SERVER);
    REQUIRE(s);
    CHECK(s->user == &other_user);
    CHECK(s->on_start == nullptr);

    // other data under the same id is a duplicate, and its callbacks are not kept
    KyubuAbilityDef e = d;
    e.duration_ms = 900;
    e.user = &start_user;
    CHECK(api->ability_define(demo.on(KY_SIDE_SERVER), "demo:burst", &e) == KY_E_DUPLICATE);
    CHECK(ability_callbacks("demo:burst", KY_SIDE_SERVER)->user == &other_user);

    // after the freeze the ability has its number, and the payload holds the data
    R.freeze(by_name);
    uint32_t n = 0;
    CHECK(R.lookup("kyubu:abilities", "demo:burst", &n) == KY_OK);
    CHECK(n == 112);
    std::vector<uint8_t> payload;
    REQUIRE(R.payload("kyubu:abilities", "demo:burst", &payload) == KY_OK);
    AbilityData back;
    REQUIRE(ability_unpack(payload.data(), payload.size(), &back));
    CHECK(back.name == "demo:burst_name");
    CHECK(back.icon == "cubeworld:smash");
    CHECK(back.description.empty());
    CHECK(back.duration_ms == 800);
    CHECK(back.mp_cost == 0.25f);
    CHECK(back.max_rank == 2);
    CHECK(back.cooldown_ms == std::vector<uint32_t>{6000, 5500, 5000});
    CHECK(back.slot == 4);
    CHECK(back.class_type == 1);
    CHECK(back.specialization == 1);
    CHECK(ability_pack(back) == payload);   // the packing is deterministic
}

TEST_CASE("ability-registry: wrong phase") {
    // covers: ability-registry/Wrong phase
    Loaded state;
    Registries& R = reset_registries();
    DemoMod demo("demo");
    const KyubuApi* api = api_table();
    KyubuAbilityDef d = good_def();
    L().phase = KY_PHASE_PREINIT;
    CHECK(api->ability_define(demo.on(KY_SIDE_CLIENT), "demo:early", &d) == KY_E_PHASE);
    L().phase = KY_PHASE_REGISTER;
    CHECK(api->ability_define(demo.on(KY_SIDE_CLIENT), "demo:ok", &d) == KY_OK);
    R.freeze(by_name);
    L().phase = KY_PHASE_POSTREGISTER;
    CHECK(api->ability_define(demo.on(KY_SIDE_CLIENT), "demo:late", &d) == KY_E_PHASE);
    uint32_t n = 0;
    CHECK(R.lookup("kyubu:abilities", "demo:late", &n) == KY_E_NOT_FOUND);
    CHECK(R.lookup("kyubu:abilities", "demo:early", &n) == KY_E_NOT_FOUND);
    CHECK(ability_count() == 113);
    CHECK(ability_callbacks("demo:late", KY_SIDE_CLIENT) == nullptr);
}

TEST_CASE("ability-registry: bad field") {
    // covers: ability-registry/Bad field
    Loaded state;
    reset_registries();
    DemoMod demo("demo");
    const KyubuApi* api = api_table();
    struct Case { const char* field; void (*change)(KyubuAbilityDef&); };
    const Case cases[] = {
        {"duration_ms", [](KyubuAbilityDef& d) { d.duration_ms = 0; }},
        {"duration_ms", [](KyubuAbilityDef& d) { d.duration_ms = KY_ABILITY_DURATION_MAX + 1; }},
        {"mp_cost", [](KyubuAbilityDef& d) { d.mp_cost = 2; }},
        {"mp_cost", [](KyubuAbilityDef& d) { d.mp_cost = -0.1f; }},
        {"mp_cost", [](KyubuAbilityDef& d) { d.mp_cost = NAN; }},
        {"max_rank", [](KyubuAbilityDef& d) { d.max_rank = 0; d.cooldown_ms = nullptr; }},
        {"max_rank", [](KyubuAbilityDef& d) { d.max_rank = KY_ABILITY_RANK_MAX + 1; d.cooldown_ms = nullptr; }},
        {"cooldown_ms", [](KyubuAbilityDef& d) { static const uint32_t c[3] = {0, KY_ABILITY_COOLDOWN_MAX + 1, 0}; d.cooldown_ms = c; }},
        {"slot", [](KyubuAbilityDef& d) { d.slot = 3; }},
        {"class", [](KyubuAbilityDef& d) { d.class_type = 0; }},
        {"class", [](KyubuAbilityDef& d) { d.class_type = 5; }},
        {"specialization", [](KyubuAbilityDef& d) { d.specialization = 2; }},
        {"specialization", [](KyubuAbilityDef& d) { d.specialization = -2; }},
        {"icon", [](KyubuAbilityDef& d) { d.icon = "demo:other"; }},
        {"icon", [](KyubuAbilityDef& d) { d.icon = "kyubu:placeholder"; }},
        {"icon", [](KyubuAbilityDef& d) { d.icon = "cubeworld:none"; }},
        {"name", [](KyubuAbilityDef& d) { static const std::string s(300, 'x'); d.name = s.c_str(); }},
        {"size", [](KyubuAbilityDef& d) { d.size = 8; }},
    };
    int i = 0;
    for (const Case& c : cases) {
        KyubuAbilityDef d = good_def();
        c.change(d);
        const uint64_t seq = Log::get().sequence();
        CAPTURE(i);
        CAPTURE(c.field);
        CHECK(api->ability_define(demo.on(KY_SIDE_CLIENT), "demo:bad", &d) == KY_E_INVALID_ARG);
        CHECK(logged(seq, std::string("ability demo:bad: ") + c.field));
        CHECK(ability_count() == 112);   // nothing was added
        ++i;
    }
    CHECK(ability_callbacks("demo:bad", KY_SIDE_CLIENT) == nullptr);
    CHECK(api->ability_define(demo.on(KY_SIDE_CLIENT), nullptr, nullptr) == KY_E_INVALID_ARG);
    KyubuAbilityDef good = good_def();
    CHECK(api->ability_define(demo.on(KY_SIDE_CLIENT), "demo:bad", &good) == KY_OK);   // the id was never taken
    CHECK(api->ability_define(demo.on(KY_SIDE_CLIENT), "demo:bad", nullptr) == KY_E_INVALID_ARG);
}

TEST_CASE("ability-registry: a text id that is not there yet is a warning") {
    Loaded state;
    reset_registries();
    DemoMod demo("demo");
    KyubuAbilityDef d = good_def();
    d.name = "demo:missing_text";
    const uint64_t seq = Log::get().sequence();
    CHECK(api_table()->ability_define(demo.on(KY_SIDE_CLIENT), "demo:texty", &d) == KY_OK);
    CHECK(logged(seq, "ability demo:texty: name 'demo:missing_text' is not in kyubu:text yet"));
}

TEST_CASE("ability-registry: another mod's namespace") {
    // covers: ability-registry/Another mod's namespace
    Loaded state;
    reset_registries();
    DemoMod a("a");
    KyubuAbilityDef d = good_def();
    CHECK(api_table()->ability_define(a.on(KY_SIDE_CLIENT), "b:thing", &d) == KY_E_NAMESPACE);
    CHECK(ability_count() == 112);
    CHECK(ability_callbacks("b:thing", KY_SIDE_CLIENT) == nullptr);
    CHECK(api_table()->ability_define(a.on(KY_SIDE_CLIENT), "a:thing", &d) == KY_OK);
}

namespace {
KyubuAbilityDef claim_def(int class_type, int specialization) {
    KyubuAbilityDef d = good_def();
    d.class_type = class_type;
    d.specialization = specialization;
    return d;
}
}  // namespace

TEST_CASE("ability-registry: specialization first") {
    // covers: ability-registry/Specialization first
    Loaded state;
    Registries& R = reset_registries();
    DemoMod a("a"), b("b");
    const KyubuApi* api = api_table();
    KyubuAbilityDef any = claim_def(1, -1), spec = claim_def(1, 0);
    CHECK(api->ability_define(a.on(KY_SIDE_CLIENT), "a:wide", &any) == KY_OK);
    CHECK(api->ability_define(b.on(KY_SIDE_CLIENT), "b:narrow", &spec) == KY_OK);   // no conflict with an `any`
    R.freeze(by_name);
    ability_slots_resolve(true);
    uint32_t wide = 0, narrow = 0;
    REQUIRE(R.lookup("kyubu:abilities", "a:wide", &wide) == KY_OK);
    REQUIRE(R.lookup("kyubu:abilities", "b:narrow", &narrow) == KY_OK);
    AbilitySlotHolder h0 = ability_slot_holder(1, 0), h1 = ability_slot_holder(1, 1);
    CHECK(h0.id == "b:narrow");
    CHECK(h0.number == narrow);
    CHECK(!h0.placeholder);
    CHECK(h0.claim == 0);    // the specialization's own claim
    CHECK(h1.id == "a:wide");
    CHECK(h1.number == wide);
    CHECK(h1.claim == -1);   // the class's `any`
}

TEST_CASE("ability-registry: second claim refused") {
    // covers: ability-registry/Second claim refused
    Loaded state;
    Registries& R = reset_registries();
    DemoMod a("a"), b("b");
    const KyubuApi* api = api_table();
    KyubuAbilityDef first = claim_def(2, -1);
    CHECK(api->ability_define(a.on(KY_SIDE_CLIENT), "a:first", &first) == KY_OK);
    CHECK(api->ability_define(a.on(KY_SIDE_SERVER), "a:first", &first) == KY_OK);   // the same ability on the other side

    KyubuAbilityDef second = claim_def(2, -1);
    second.duration_ms = 700;
    const uint64_t seq = Log::get().sequence();
    CHECK(api->ability_define(b.on(KY_SIDE_CLIENT), "b:second", &second) == KY_E_CONFLICT);
    CHECK(logged(seq, "ability b:second: slot claim (mage any) is held by a:first of mod a"));
    CHECK(logged(seq, "b:second of mod b is not defined"));
    CHECK(ability_count() == 113);
    CHECK(ability_callbacks("b:second", KY_SIDE_CLIENT) == nullptr);

    // a specialization claim of the same pair twice conflicts too; a claim of another pair and a claimless ability do not
    KyubuAbilityDef spec = claim_def(2, 1), other_spec = claim_def(2, 0), none = good_def();
    none.slot = KY_ABILITY_SLOT_NONE;
    CHECK(api->ability_define(a.on(KY_SIDE_CLIENT), "a:spec", &spec) == KY_OK);
    CHECK(api->ability_define(b.on(KY_SIDE_CLIENT), "b:spec", &spec) == KY_E_CONFLICT);
    CHECK(api->ability_define(b.on(KY_SIDE_CLIENT), "b:other", &other_spec) == KY_OK);
    CHECK(api->ability_define(b.on(KY_SIDE_CLIENT), "b:free", &none) == KY_OK);
    CHECK(api->ability_define(b.on(KY_SIDE_CLIENT), "b:free2", &none) == KY_OK);
    // the error of the namespace comes first
    CHECK(api->ability_define(b.on(KY_SIDE_CLIENT), "a:stolen", &spec) == KY_E_NAMESPACE);

    R.freeze(by_name);
    ability_slots_resolve(true);
    CHECK(ability_slot_holder(2, 0).id == "b:other");
    CHECK(ability_slot_holder(2, 1).id == "a:spec");
}

TEST_CASE("ability-registry: placeholder yields") {
    // covers: ability-registry/Placeholder yields
    Loaded state;
    Registries& R = reset_registries();
    DemoMod a("a");
    KyubuAbilityDef sneak = claim_def(4, -1);
    CHECK(api_table()->ability_define(a.on(KY_SIDE_CLIENT), "a:sneak", &sneak) == KY_OK);
    R.freeze(by_name);
    ability_slots_resolve(true);
    for (int s = 0; s < 2; ++s) {
        CHECK(ability_slot_holder(4, s).id == "a:sneak");
        CHECK(!ability_slot_holder(4, s).placeholder);
    }
    for (int c = 1; c <= 3; ++c)
        for (int s = 0; s < 2; ++s) {
            AbilitySlotHolder h = ability_slot_holder(c, s);
            CHECK(h.id == "kyubu:placeholder");
            CHECK(h.number == 111);
            CHECK(h.placeholder);
            CHECK(h.claim == -2);
        }
    ability_slots_resolve(false);   // [abilities] placeholder = false: the other classes have nothing
    CHECK(ability_slot_holder(4, 0).id == "a:sneak");
    CHECK(ability_slot_holder(1, 0).id.empty());
    CHECK(ability_slot_holder(0, 0).id.empty());   // out of range
    CHECK(ability_slot_holder(1, 2).id.empty());
    CHECK(ability_slot_holder(5, 0).id.empty());
}

namespace {
std::string line_with(const std::vector<std::string>& lines, const std::string& part) {
    for (auto& l : lines)
        if (l.find(part) != std::string::npos) return l;
    return "";
}
}  // namespace

TEST_CASE("ability-registry: number, name and list") {
    // covers: ability-registry/Vanilla names (through the API)
    // covers: ability-registry/Mod numbers follow the load order (through the API)
    Loaded state;
    Registries& R = reset_registries();
    DemoMod a("a"), b("b");
    const KyubuApi* api = api_table();
    KyubuAbilityDef plain = good_def(), claim = claim_def(3, 0);
    plain.slot = KY_ABILITY_SLOT_NONE;
    CHECK(api->ability_define(b.on(KY_SIDE_CLIENT), "b:claimer", &claim) == KY_OK);
    CHECK(api->ability_define(a.on(KY_SIDE_CLIENT), "a:plain", &plain) == KY_OK);

    // before the freeze: vanilla and the placeholder answer; a mod's ability and the list do not
    uint32_t n = 0, count = 0;
    char buf[64];
    CHECK(api->ability_number("cubeworld:smash", &n) == KY_OK);
    CHECK(n == 54);
    CHECK(api->ability_number("kyubu:placeholder", &n) == KY_OK);
    CHECK(n == 111);
    CHECK(api->ability_number("a:plain", &n) == KY_E_PHASE);
    CHECK(api->ability_name(112, buf, sizeof buf) == KY_E_NOT_FOUND);
    CHECK(api->ability_list(0, buf, sizeof buf, &n, &count) == KY_E_PHASE);

    R.freeze(by_name);
    CHECK(api->ability_number("a:plain", &n) == KY_OK);
    CHECK(n == 112);
    CHECK(api->ability_number("b:claimer", &n) == KY_OK);
    CHECK(n == 113);
    CHECK(api->ability_number("a:nothing", &n) == KY_E_NOT_FOUND);
    CHECK(api->ability_number(nullptr, &n) == KY_E_INVALID_ARG);
    CHECK(api->ability_number("a:plain", nullptr) == KY_E_INVALID_ARG);
    CHECK(api->ability_name(54, buf, sizeof buf) == KY_OK);
    CHECK(std::string(buf) == "cubeworld:smash");
    CHECK(api->ability_name(113, buf, sizeof buf) == KY_OK);
    CHECK(std::string(buf) == "b:claimer");
    CHECK(api->ability_name(114, buf, sizeof buf) == KY_E_NOT_FOUND);
    char tiny[4];
    CHECK(api->ability_name(54, tiny, sizeof tiny) == KY_E_BUFFER);
    CHECK(std::string(tiny) == "cub");   // cut and terminated

    // the list: every ability in number order; the count comes with any index
    CHECK(api->ability_list(0, buf, sizeof buf, &n, &count) == KY_OK);
    CHECK(count == 114);
    for (uint32_t i = 0; i < count; ++i) {
        REQUIRE(api->ability_list(i, buf, sizeof buf, &n, nullptr) == KY_OK);
        CHECK(n == i);
        char by_number[64];
        REQUIRE(api->ability_name(n, by_number, sizeof by_number) == KY_OK);
        CHECK(std::string(buf) == by_number);
    }
    CHECK(std::string(buf) == "b:claimer");
    count = 0;
    CHECK(api->ability_list(114, buf, sizeof buf, &n, &count) == KY_E_NOT_FOUND);
    CHECK(count == 114);
    CHECK(api->ability_list(0, nullptr, 0, &n, nullptr) == KY_OK);   // the id is optional
    CHECK(api->ability_list(54, tiny, sizeof tiny, &n, nullptr) == KY_E_BUFFER);
}

TEST_CASE("ability-registry: the owner of slot 4") {
    // covers: ability-registry/Specialization first (through the API)
    // covers: ability-registry/Placeholder yields (through the API)
    Loaded state;
    Registries& R = reset_registries();
    DemoMod a("a");
    const KyubuApi* api = api_table();
    KyubuAbilityDef spec = claim_def(1, 1), any = claim_def(2, -1);
    CHECK(api->ability_define(a.on(KY_SIDE_CLIENT), "a:guard", &spec) == KY_OK);
    CHECK(api->ability_define(a.on(KY_SIDE_CLIENT), "a:wide", &any) == KY_OK);
    char buf[64];
    uint32_t n = 0;
    // no table yet: a phase error for good arguments, an invalid-argument error for bad ones
    CHECK(api->ability_slot_owner(1, 1, buf, sizeof buf, &n) == KY_E_PHASE);
    CHECK(api->ability_slot_owner(0, 1, buf, sizeof buf, &n) == KY_E_INVALID_ARG);
    R.freeze(by_name);
    CHECK(api->ability_slot_owner(1, 1, buf, sizeof buf, &n) == KY_E_PHASE);   // frozen, table not built

    ability_slots_resolve(true);
    CHECK(api->ability_slot_owner(1, 1, buf, sizeof buf, &n) == KY_OK);
    CHECK(std::string(buf) == "a:guard");
    CHECK(n == 112);
    CHECK(api->ability_slot_owner(2, 0, buf, sizeof buf, &n) == KY_OK);
    CHECK(std::string(buf) == "a:wide");
    CHECK(n == 113);
    CHECK(api->ability_slot_owner(2, 1, buf, sizeof buf, &n) == KY_OK);
    CHECK(std::string(buf) == "a:wide");
    CHECK(api->ability_slot_owner(3, 0, buf, sizeof buf, &n) == KY_OK);
    CHECK(std::string(buf) == "kyubu:placeholder");
    CHECK(n == 111);
    CHECK(api->ability_slot_owner(1, 0, nullptr, 0, nullptr) == KY_OK);   // id and number are optional
    char tiny[4];
    CHECK(api->ability_slot_owner(1, 1, tiny, sizeof tiny, &n) == KY_E_BUFFER);
    // a character's specialization is 0 or 1: `any` is only a claim
    CHECK(api->ability_slot_owner(1, KY_ABILITY_SPEC_ANY, buf, sizeof buf, &n) == KY_E_INVALID_ARG);
    CHECK(api->ability_slot_owner(1, 2, buf, sizeof buf, &n) == KY_E_INVALID_ARG);
    CHECK(api->ability_slot_owner(0, 0, buf, sizeof buf, &n) == KY_E_INVALID_ARG);
    CHECK(api->ability_slot_owner(5, 0, buf, sizeof buf, &n) == KY_E_INVALID_ARG);

    ability_slots_resolve(false);   // [abilities] placeholder = false
    CHECK(api->ability_slot_owner(3, 0, buf, sizeof buf, &n) == KY_E_NOT_FOUND);
    CHECK(api->ability_slot_owner(2, 0, buf, sizeof buf, &n) == KY_OK);
}

TEST_CASE("ability-registry: the lines of ability list and ability slots") {
    // covers: ability-registry/Slots with the placeholder (the lines; the menu replay shows them in the game)
    Loaded state;
    Registries& R = reset_registries();
    DemoMod demo("a");
    const KyubuApi* api = api_table();
    KyubuAbilityDef burst = claim_def(1, -1), plain = good_def();
    plain.slot = KY_ABILITY_SLOT_NONE;
    CHECK(api->ability_define(demo.on(KY_SIDE_CLIENT), "a:burst", &burst) == KY_OK);
    CHECK(api->ability_define(demo.on(KY_SIDE_CLIENT), "a:plain", &plain) == KY_OK);

    CHECK(ability_list_lines().size() == 1);   // one line: not numbered yet
    CHECK(line_with(ability_list_lines(), "not yet") != "");
    CHECK(line_with(ability_slot_lines(), "not yet") != "");
    R.freeze(by_name);
    CHECK(line_with(ability_slot_lines(), "not yet") != "");   // the table comes right after the freeze

    ability_slots_resolve(true);
    // the scenario: the warrior's two specializations show the `any` claim, the other six pairs the placeholder
    const auto slots = ability_slot_lines();
    CHECK(slots.size() == 8);
    CHECK(line_with(slots, "warrior  specialization 0: a:burst (112), claim: any") != "");
    CHECK(line_with(slots, "warrior  specialization 1: a:burst (112), claim: any") != "");
    CHECK(line_with(slots, "rogue    specialization 1: kyubu:placeholder (111), placeholder") != "");
    size_t placeholders = 0;
    for (auto& l : slots) placeholders += l.find("kyubu:placeholder (111), placeholder") != std::string::npos;
    CHECK(placeholders == 6);
    ability_slots_resolve(false);
    CHECK(line_with(ability_slot_lines(), "mage     specialization 0: nobody") != "");

    // the list: a header, then one line per ability with number, id, owner and claim
    const auto list = ability_list_lines();
    CHECK(list.size() == 1 + ability_count());
    const std::string burst_line = line_with(list, "a:burst");
    CHECK(burst_line.find("112") != std::string::npos);
    REQUIRE(burst_line.find(" a ") != std::string::npos);
    CHECK(burst_line.find("a:burst") < burst_line.find(" a "));   // the id, then the owning mod
    CHECK(burst_line.find("slot 4: warrior any") != std::string::npos);
    const std::string plain_line = line_with(list, "a:plain");
    CHECK(plain_line.size() > 2);
    CHECK(plain_line.compare(plain_line.size() - 2, 2, " -") == 0);   // no claim
    const std::string smash_line = line_with(list, "cubeworld:smash");
    CHECK(smash_line.find(" 54 ") != std::string::npos);
    CHECK(smash_line.compare(smash_line.size() - 2, 2, " -") == 0);
    CHECK(line_with(list, "  111  kyubu:placeholder") != "");
}

TEST_CASE("ability-registry: class and specialization names") {
    CHECK(ability_class_value("warrior") == 1);
    CHECK(ability_class_value("Rogue") == 4);
    CHECK(ability_class_value("paladin") == -2);
    CHECK(ability_specialization_value(1, "guardian") == 1);
    CHECK(ability_specialization_value(1, "berserker") == 0);
    CHECK(ability_specialization_value(4, "ninja") == 1);
    CHECK(ability_specialization_value(1, "ninja") == -2);   // the rogue's, not the warrior's
    CHECK(ability_specialization_value(2, "any") == -1);
}

TEST_CASE("ability-registry: a restricted mod has no ability API") {
    // covers: ability-registry/A restricted mod is refused
    Loaded state;
    Registries& R = reset_registries();
    DemoMod demo("demo");
    demo.mod.manifest.trust = "restricted";
    KyubuAbilityDef d = good_def();
    const uint64_t seq = Log::get().sequence();
    CHECK(api_table()->ability_define(demo.on(KY_SIDE_CLIENT), "demo:burst", &d) == KY_E_TRUST);
    CHECK(logged(seq, "ability_define needs trust \"full\""));
    uint32_t n = 0;
    CHECK(R.lookup("kyubu:abilities", "demo:burst", &n) == KY_E_NOT_FOUND);
    demo.mod.manifest.trust = "full";
    CHECK(api_table()->ability_define(demo.on(KY_SIDE_CLIENT), "demo:burst", &d) == KY_OK);

    // Lua: a restricted mod has no kyubu.ability at all
    Loader& l = L();
    const std::string saved_kyubu = l.kyubu_dir, saved_mods = l.mods_dir;
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::filesystem::path p = std::filesystem::path(tmp) / ("kyubu-ability-restricted-" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    std::filesystem::create_directories(p);
    const std::string game_dir = p.string();
    l.kyubu_dir = game_dir + "/kyubu";
    l.mods_dir = game_dir + "/mods";
    const std::string mod = l.mods_dir + "/fx_restricted";
    REQUIRE(make_dirs(mod + "/scripts"));
    REQUIRE(write_text(mod + "/mod.toml", "id = \"fx_restricted\"\nname = \"Abilities: restricted\"\n"
                                          "version = \"1.0.0\"\nkyubu = \"^1.0\"\nsides = [\"client\"]\ntrust = \"restricted\"\n"
                                          "description = \"Unit test: no kyubu.ability at restricted trust.\"\n\n[scripts]\nclient = \"scripts/client.lua\"\n"));
    REQUIRE(write_text(mod + "/scripts/client.lua", "kyubu.info('fx_restricted: ability=' .. type(kyubu.ability))\n"));
    const uint64_t seq2 = Log::get().sequence();
    auto& mm = ModManager::get();
    mm.discover_and_resolve();
    mm.load_native();
    LuaRuntime::get().load_all();
    CHECK(logged(seq2, "fx_restricted: ability=nil"));
    reset_registries();
    l.mods_dir = game_dir + "/empty";
    mm.discover_and_resolve();
    l.kyubu_dir = saved_kyubu;
    l.mods_dir = saved_mods;
}

static void KYUBU_CALL rec_step_any(void*, const KyubuAbilityCtx*) {}

TEST_CASE("ability-registry: a full-trust Lua mod defines abilities") {
    // covers: ability-registry/A full-trust mod defines an ability
    // covers: ability-registry/Bad field (the Lua half)
    // covers: ability-registry/Another mod's namespace (the Lua half)
    Loaded state;
    Registries& R = reset_registries();
    Loader& l = L();
    const std::string saved_kyubu = l.kyubu_dir, saved_mods = l.mods_dir;

    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    auto temp = [&](const char* name) {
        std::filesystem::path p = std::filesystem::path(tmp) / (std::string("kyubu-ability-") + name + "-" + std::to_string(GetCurrentProcessId()));
        std::error_code ec;
        std::filesystem::remove_all(p, ec);
        std::filesystem::create_directories(p);
        return p.string();
    };
    const std::string game_dir = temp("lua");
    l.kyubu_dir = game_dir + "/kyubu";
    l.mods_dir = game_dir + "/mods";
    const std::string mod = l.mods_dir + "/fx_ability";
    REQUIRE(make_dirs(mod + "/scripts"));
    REQUIRE(write_text(mod + "/mod.toml", "id = \"fx_ability\"\nname = \"Abilities: define from Lua\"\n"
                                          "version = \"1.0.0\"\nkyubu = \"^1.0\"\nsides = [\"client\"]\ntrust = \"full\"\n"
                                          "description = \"Unit test: kyubu.ability.define.\"\n\n[scripts]\nclient = \"scripts/client.lua\"\n"));
    REQUIRE(write_text(mod + "/scripts/client.lua", R"(
local r = {}
local function note(name, ok, err) r[#r + 1] = name .. "=" .. tostring(ok) .. (err and ("/" .. tostring(err)) or "") end
kyubu.text.add("fx_ability:ramp_name", "Ramp")
note("burst", kyubu.ability.define("fx_ability:burst", { duration_ms = 800, cooldown_ms = 5000 }))
note("ramp", kyubu.ability.define("fx_ability:ramp", {
  name = "fx_ability:ramp_name", icon = "cubeworld:smash", duration_ms = 800, mp_cost = 0.25, max_rank = 2,
  cooldown_ms = function(rank) return 6000 - 500 * rank end,
  claim = { slot = 4, class = "warrior", specialization = "guardian" },
  on_start = function(ctx) end, on_step = function(ctx) end, on_end = function(ctx, reason) end,
  watch = { on_step = function(ctx) end }, restarts = 1 }))
note("same_again", kyubu.ability.define("fx_ability:burst", { duration_ms = 800, cooldown_ms = 5000 }))
note("other_data", kyubu.ability.define("fx_ability:burst", { duration_ms = 801, cooldown_ms = 5000 }))
note("zero_duration", kyubu.ability.define("fx_ability:bad1", { duration_ms = 0 }))
note("mp_two", kyubu.ability.define("fx_ability:bad2", { mp_cost = 2 }))
note("slot_three", kyubu.ability.define("fx_ability:bad3", { claim = { slot = 3, class = "mage" } }))
note("slot_zero", kyubu.ability.define("fx_ability:bad9", { claim = { slot = 0, class = "mage" } }))
local half_ok = pcall(kyubu.ability.define, "fx_ability:bad10", { claim = { slot = 4, class = 1.5 } })
r[#r + 1] = "half_class=" .. tostring(half_ok)
note("unknown_class", kyubu.ability.define("fx_ability:bad4", { claim = { slot = 4, class = "paladin" } }))
note("cooldown_raises", kyubu.ability.define("fx_ability:bad5", { max_rank = 2, cooldown_ms = function(rank) error("boom") end }))
note("big_cooldown", kyubu.ability.define("fx_ability:bad6", { cooldown_ms = 9999999 }))
note("many_restarts", kyubu.ability.define("fx_ability:bad13", { restarts = 16 }))
note("namespace", kyubu.ability.define("other:thing", {}))
local ok, err = pcall(kyubu.ability.define, "fx_ability:bad7", { duration_ms = "long" })
r[#r + 1] = "string_duration=" .. tostring(ok) .. "/" .. tostring(err):gsub("^.-:%d+: ", "")
ok, err = pcall(kyubu.ability.define, "fx_ability:bad8", { on_start = 5 })
r[#r + 1] = "bad_callback=" .. tostring(ok) .. "/" .. tostring(err):gsub("^.-:%d+: ", "")
ok, err = pcall(kyubu.ability.define, "fx_ability:bad11", { watch = 5 })
r[#r + 1] = "bad_watch=" .. tostring(ok) .. "/" .. tostring(err):gsub("^.-:%d+: ", "")
ok, err = pcall(kyubu.ability.define, "fx_ability:bad12", { watch = { on_end = 5 } })
r[#r + 1] = "bad_watch_end=" .. tostring(ok) .. "/" .. tostring(err):gsub("^.-:%d+: ", "")
-- a mod's number, the list and the slot table exist only after the freeze
note("number_early", kyubu.ability.number("fx_ability:burst"))
note("list_early", kyubu.ability.list())
note("owner_early", kyubu.ability.slot_owner("warrior", 1))
kyubu.command("fx_ability:query", "unit test", function(args, print)
  print("number", kyubu.ability.number("fx_ability:burst"))
  print("number_vanilla", kyubu.ability.number("cubeworld:smash"))
  print("number_unknown", kyubu.ability.number("fx_ability:nope"))
  print("name", kyubu.ability.name(113))
  print("name_unknown", kyubu.ability.name(9999))
  local l = kyubu.ability.list()
  print("list", #l, l[#l].id, l[#l].number)
  print("owner", kyubu.ability.slot_owner("warrior", 1))
  print("owner_name", kyubu.ability.slot_owner("warrior", "guardian"))
  print("owner_placeholder", kyubu.ability.slot_owner("mage", 0))
  print("owner_bad", kyubu.ability.slot_owner("paladin", 0))
  print("owner_bad_spec", kyubu.ability.slot_owner(1, 2))
end)
kyubu.info("fx_ability: " .. table.concat(r, " | "))
)"));

    const uint64_t seq = Log::get().sequence();
    auto& mm = ModManager::get();
    mm.discover_and_resolve();
    mm.load_native();
    LuaRuntime::get().load_all();
    std::string line;
    for (auto& entry : Log::get().since(seq))
        if (entry.text.find("fx_ability: ") != std::string::npos && entry.text.find("burst=") != std::string::npos) line = entry.text;
    Mod* m = mm.find("fx_ability");
    REQUIRE(m);
    CHECK(m->state == ModState::Loaded);
    CHECK_MESSAGE(!line.empty(), "the script did not log its results");
    auto has = [&](const char* part) { CHECK_MESSAGE(line.find(part) != std::string::npos, part, " in: ", line); };
    has("burst=true");
    has("ramp=true");
    has("same_again=true");
    has("other_data=nil/already registered");
    has("zero_duration=nil/invalid argument");
    has("mp_two=nil/invalid argument");
    has("slot_three=nil/invalid argument");
    has("slot_zero=nil/invalid argument");
    has("half_class=false");
    has("unknown_class=nil/invalid argument");
    has("cooldown_raises=nil/invalid argument");
    has("big_cooldown=nil/invalid argument");
    has("many_restarts=nil/invalid argument");
    has("namespace=nil/outside the mod's namespace");
    has("string_duration=false/kyubu.ability.define: 'duration_ms' must be a number");
    has("bad_callback=false/kyubu.ability.define: 'on_start' must be a function");
    has("bad_watch=false/kyubu.ability.define: 'watch' must be a table");
    has("bad_watch_end=false/kyubu.ability.define: 'watch.on_end' must be a function");
    // the watch callbacks (6.2) reach the loader with the rest; an ability without them is not watched
    {
        const AbilityCallbacks* ramp = ability_callbacks("fx_ability:ramp", KY_SIDE_CLIENT);
        REQUIRE(ramp);
        CHECK(ramp->on_step);
        CHECK(ramp->watch_step);
        CHECK(ramp->watch_user);
        CHECK(ramp->watch_user != ramp->user);   // a watched run keeps its own ctx.state
        CHECK(ramp->restarts == 1);               // 8.1: the restart count reaches the loader too
        const AbilityCallbacks* burst = ability_callbacks("fx_ability:burst", KY_SIDE_CLIENT);
        const bool burst_watched = burst && burst->watch_step;
        CHECK_FALSE(burst_watched);
    }
    has("number_early=nil/not allowed in this lifecycle phase");
    has("list_early=nil/not allowed in this lifecycle phase");
    has("owner_early=nil/not allowed in this lifecycle phase");
    CHECK(logged(seq, "ability fx_ability:bad1: duration_ms must be 1 to 60000"));
    CHECK(logged(seq, "ability fx_ability:bad2: mp_cost must be 0 to 1"));
    CHECK(logged(seq, "ability fx_ability:bad3: slot must be 4"));
    CHECK(logged(seq, "ability fx_ability:bad9: slot must be 4"));
    CHECK(logged(seq, "ability fx_ability:bad4: class of the claim"));
    // the text the script added in this phase is known: no warning for it (the name of `ramp`)
    CHECK_FALSE(logged(seq, "fx_ability:ramp_name' is not in kyubu:text"));
    CHECK(logged(seq, "ability fx_ability:bad5: cooldown_ms failed for rank 0"));
    CHECK(logged(seq, "ability fx_ability:bad6: cooldown_ms of rank 0"));

    // after the freeze: burst is the first mod ability (ids sort within the mod), and only the good ones exist
    R.freeze(by_name);
    uint32_t n = 0;
    CHECK(R.lookup("kyubu:abilities", "fx_ability:burst", &n) == KY_OK);
    CHECK(n == 112);
    CHECK(R.lookup("kyubu:abilities", "fx_ability:ramp", &n) == KY_OK);
    CHECK(n == 113);
    for (const char* id : {"fx_ability:bad1", "fx_ability:bad2", "fx_ability:bad3", "fx_ability:bad4", "fx_ability:bad5",
                           "fx_ability:bad6", "fx_ability:bad7", "fx_ability:bad8", "fx_ability:bad9", "fx_ability:bad10",
                           "fx_ability:bad11", "fx_ability:bad12", "other:thing"})
        CHECK(R.lookup("kyubu:abilities", id, &n) == KY_E_NOT_FOUND);
    CHECK(ability_count() == 114);

    // the cooldown function became a table, and the claim names became numbers
    std::vector<uint8_t> payload;
    REQUIRE(R.payload("kyubu:abilities", "fx_ability:ramp", &payload) == KY_OK);
    AbilityData d;
    REQUIRE(ability_unpack(payload.data(), payload.size(), &d));
    CHECK(d.cooldown_ms == std::vector<uint32_t>{6000, 5500, 5000});
    CHECK(d.max_rank == 2);
    CHECK(d.mp_cost == 0.25f);
    CHECK(d.slot == 4);
    CHECK(d.class_type == 1);
    CHECK(d.specialization == 1);
    CHECK(d.name == "fx_ability:ramp_name");
    CHECK(d.icon == "cubeworld:smash");
    CHECK(d.duration_ms == 800);

    // the Lua side of number, name, list and slot_owner, once the numbers and the slot table exist
    ability_slots_resolve(true);
    std::string query;
    Console::get().execute("fx_ability:query", [&](const std::string& s) { query += s + "\n"; });
    Console::get().remove_mod(m);
    auto saw = [&](const char* part) { CHECK_MESSAGE(query.find(part) != std::string::npos, part, " in: ", query); };
    saw("number 112\n");
    saw("number_vanilla 54\n");
    saw("number_unknown nil not found\n");
    saw("name fx_ability:ramp\n");
    saw("name_unknown nil not found\n");
    saw("list 114 fx_ability:ramp 113\n");
    saw("owner fx_ability:ramp 113\n");
    saw("owner_name fx_ability:ramp 113\n");
    saw("owner_placeholder kyubu:placeholder 111\n");
    saw("owner_bad nil invalid argument\n");
    saw("owner_bad_spec nil invalid argument\n");

    // native and Lua agree on the bytes: the same data from a native mod packs to the same payload
    reset_registries();
    DemoMod native("fx_ability");
    static const uint32_t cooldown[3] = {6000, 5500, 5000};
    KyubuAbilityDef nd = {};
    nd.size = sizeof nd;
    nd.name = "fx_ability:ramp_name";
    nd.icon = "cubeworld:smash";
    nd.duration_ms = 800;
    nd.mp_cost = 0.25f;
    nd.max_rank = 2;
    nd.cooldown_ms = cooldown;
    nd.slot = KY_ABILITY_SLOT_FOURTH;
    nd.class_type = 1;
    nd.specialization = 1;
    CHECK(api_table()->ability_define(native.on(KY_SIDE_CLIENT), "fx_ability:ramp", &nd) == KY_OK);
    std::vector<uint8_t> native_payload;
    Registries::get().freeze(by_name);
    REQUIRE(Registries::get().payload("kyubu:abilities", "fx_ability:ramp", &native_payload) == KY_OK);
    CHECK(native_payload == payload);

    // a caller built before the watch members (a smaller size) keeps its icon file and is not watched, whatever follows in memory
    reset_registries();
    DemoMod older("fx_ability");
    KyubuAbilityDef od = nd;
    od.icon_file = "ramp.png";
    od.watch_step = rec_step_any;
    od.size = (uint32_t)offsetof(KyubuAbilityDef, watch_start);
    CHECK(api_table()->ability_define(older.on(KY_SIDE_CLIENT), "fx_ability:ramp", &od) == KY_OK);
    const AbilityCallbacks* oc = ability_callbacks("fx_ability:ramp", KY_SIDE_CLIENT);
    REQUIRE(oc);
    CHECK(oc->watch_step == nullptr);
    Registries::get().freeze(by_name);
    std::vector<uint8_t> older_payload;
    REQUIRE(Registries::get().payload("kyubu:abilities", "fx_ability:ramp", &older_payload) == KY_OK);
    AbilityData odd;
    REQUIRE(ability_unpack(older_payload.data(), older_payload.size(), &odd));
    CHECK(odd.icon_file == "ramp.png");

    // leave no mods behind for other tests
    reset_registries();
    l.mods_dir = temp("lua-empty");
    mm.discover_and_resolve();
    l.kyubu_dir = saved_kyubu;
    l.mods_dir = saved_mods;
}

TEST_CASE("ability-registry: defining works while the fourth slot is off") {
    // covers: fourth-ability-slot/A candidate entry (the definitions half): no symbol map, so no primitive is available
    Loaded state;
    reset_registries();
    SymbolTable::get().clear();
    game::check_entries();
    std::string why;
    CHECK(!game::ability_slot_available(&why));
    CHECK_MESSAGE(why.find("ability_state") == 0, why);
    DemoMod demo("demo");
    KyubuAbilityDef d = good_def();
    CHECK(api_table()->ability_define(demo.on(KY_SIDE_CLIENT), "demo:off", &d) == KY_OK);
}

namespace {

// A fake of the game's push_back on a plain buffer: the bar the tests fill.
struct FakeBar {
    int ids[16] = {};
    std_vector v = {};
    int pushes = 0;
    explicit FakeBar(std::initializer_list<int> init) {
        int n = 0;
        for (int i : init) ids[n++] = i;
        v.first = ids;
        v.last = ids + n;
        v.end = ids + 16;
    }
    int size() const { return (int)((int*)v.last - (int*)v.first); }
};
FakeBar* g_fake = nullptr;
void fake_push(std_vector* bar, const int* value) {
    REQUIRE(g_fake);
    REQUIRE(bar == &g_fake->v);
    *(int*)bar->last = *value;
    bar->last = (int*)bar->last + 1;
    ++g_fake->pushes;
}
uint32_t g_rank = 1;
uint32_t rank_now(uint32_t) { return g_rank; }

struct BarRig {
    Loaded state;
    BarRig() {
        Registries& R = reset_registries();
        DemoMod a("a");
        KyubuAbilityDef spec = claim_def(1, 1), any = claim_def(2, -1);
        REQUIRE(api_table()->ability_define(a.on(KY_SIDE_CLIENT), "a:guard", &spec) == KY_OK);
        REQUIRE(api_table()->ability_define(a.on(KY_SIDE_CLIENT), "a:wide", &any) == KY_OK);
        R.freeze(by_name);
        ability_slots_resolve(true);
        g_rank = 1;
        game::set_rank_source_for_test(rank_now);
        game::reset_bar_log_for_test();
    }
    ~BarRig() {
        game::set_rank_source_for_test(nullptr);
        g_fake = nullptr;
        reset_registries();
    }
};

}  // namespace

TEST_CASE("fourth-ability-slot: the push onto vanilla's bar") {
    // covers: fourth-ability-slot/Warrior slot 4 (the unit-test half: the owner's number is pushed after the five entries)
    BarRig rig;
    // warrior 1, specialization 1 holds a:guard (112); warrior 0 has no claim and gets the placeholder (111); the ranger's
    // `any` claim is a:wide (113)
    FakeBar bar{57, 59, 54, 86, 102};
    g_fake = &bar;
    CHECK(game::bar_fill(&bar.v, 1, 1, fake_push) == game::BarFill::Pushed);
    CHECK(bar.size() == 6);
    CHECK(bar.ids[5] == 112);
    CHECK(bar.ids[0] == 57);   // the other five are untouched
    CHECK(bar.ids[4] == 102);

    FakeBar plain{57, 59, 54, 86, 102};
    g_fake = &plain;
    CHECK(game::bar_fill(&plain.v, 1, 0, fake_push) == game::BarFill::Pushed);
    CHECK(plain.ids[5] == 111);

    FakeBar any{40, 59, 21, 50, 99};
    g_fake = &any;
    CHECK(game::bar_fill(&any.v, 2, 0, fake_push) == game::BarFill::Pushed);
    CHECK(any.ids[5] == 113);
}

TEST_CASE("fourth-ability-slot: a rank of 0 leaves the slot empty") {
    // covers: fourth-ability-slot/Not learned (the unit-test half)
    BarRig rig;
    g_rank = 0;
    FakeBar bar{57, 59, 54, 86, 102};
    g_fake = &bar;
    CHECK(game::bar_fill(&bar.v, 1, 1, fake_push) == game::BarFill::NotLearned);
    CHECK(bar.size() == 5);
    CHECK(bar.pushes == 0);
    g_rank = 3;
    CHECK(game::bar_fill(&bar.v, 1, 1, fake_push) == game::BarFill::Pushed);
    CHECK(bar.size() == 6);
}

namespace {

// A fake of the HUD's icon map: nodes laid out as the game's (std_tree_node, then the int key at +0x10 and the 4-byte value
// at +0x14), under a head that is also the nil node.
struct IconNode {
    std_tree_node t;
    int key;
    uint32_t value;
};
struct FakeIconMap {
    std_tree_node head = {};
    IconNode n54{}, n0{}, n115{};
    std_map m = {};
    FakeIconMap(bool with_115 = true, uint32_t smash = 0x1000) {
        head.isnil = 1;
        head.left = head.right = head.parent = &head;
        link(n54, 54, smash);
        link(n0, 0, 0x2000);
        n54.t.left = &n0.t;
        n0.t.parent = &n54.t;
        head.parent = &n54.t;
        n54.t.parent = &head;
        if (with_115) {
            link(n115, 115, 0);
            n54.t.right = &n115.t;
            n115.t.parent = &n54.t;
        }
        m.head = &head;
        m.size = with_115 ? 3 : 2;
    }
    void link(IconNode& n, int key, uint32_t value) {
        n.t = {};
        n.t.left = n.t.right = &head;
        n.key = key;
        n.value = value;
    }
};
static_assert(offsetof(IconNode, key) == 0x10 && offsetof(IconNode, value) == 0x14, "the fake follows the game's node");

}  // namespace

TEST_CASE("fourth-ability-slot: the icon map") {
    // covers: fourth-ability-slot/Warrior slot 4 (the unit-test half: the slot shows the named vanilla ability's icon)
    SUBCASE("the node of the number gets the value of the vanilla ability's node") {
        FakeIconMap map;
        CHECK(game::icon_fill(&map.m, 115, 54) == game::IconFill::Written);
        CHECK(map.n115.value == 0x1000);
        CHECK(map.n54.value == 0x1000);   // the source is untouched
        CHECK(map.m.size == 3);           // no node was added
        CHECK(game::icon_fill(&map.m, 115, 0) == game::IconFill::AlreadySet);
        CHECK(map.n115.value == 0x1000);
    }
    SUBCASE("no node yet: nothing is written and nothing is inserted") {
        FakeIconMap map(false);
        CHECK(game::icon_fill(&map.m, 115, 54) == game::IconFill::NoNode);
        CHECK(map.m.size == 2);
        CHECK(map.n54.value == 0x1000);
        CHECK(map.n0.value == 0x2000);
    }
    SUBCASE("a source that is missing or null leaves the slot empty") {
        FakeIconMap map;
        CHECK(game::icon_fill(&map.m, 115, 57) == game::IconFill::NoSource);   // 57 has no node here
        CHECK(map.n115.value == 0);
        FakeIconMap blank(true, 0);
        CHECK(game::icon_fill(&blank.m, 115, 54) == game::IconFill::NoSource);
        CHECK(blank.n115.value == 0);
    }
    SUBCASE("an empty map, and a map that is not a tree") {
        std_map none = {};
        CHECK(game::icon_fill(&none, 115, 54) == game::IconFill::NoNode);
        FakeIconMap loop;
        loop.n115.t.parent = nullptr;
        loop.n54.t.right = &loop.n54.t;   // 115 > 54: the walk goes round the root forever
        CHECK(game::icon_fill(&loop.m, 115, 54) == game::IconFill::NotATree);
        CHECK(loop.n115.value == 0);
    }
}

TEST_CASE("fourth-ability-slot: the stat row carries the icon's number") {
    Loaded state;
    Registries& R = reset_registries();
    DemoMod a("a");
    KyubuAbilityDef with = claim_def(1, 1), without = claim_def(2, -1);
    with.icon = "cubeworld:smash";
    REQUIRE(api_table()->ability_define(a.on(KY_SIDE_CLIENT), "a:iconic", &with) == KY_OK);
    REQUIRE(api_table()->ability_define(a.on(KY_SIDE_CLIENT), "a:plain", &without) == KY_OK);
    R.freeze(by_name);
    ability_slots_resolve(true);
    const AbilityStat* iconic = ability_stat(112);
    const AbilityStat* plain = ability_stat(113);
    REQUIRE(iconic);
    REQUIRE(plain);
    CHECK(iconic->icon == 54);
    CHECK(plain->icon == 0);
    REQUIRE(ability_stat(111));
    CHECK(ability_stat(111)->icon == 54);   // the placeholder's stand-in icon: cubeworld:smash's
    reset_registries();
}

TEST_CASE("fourth-ability-slot: nobody holds the slot") {
    BarRig rig;
    SUBCASE("a pair that is not a class") {
        FakeBar bar{57, 59, 54, 86, 102};
        g_fake = &bar;
        CHECK(game::bar_fill(&bar.v, 0, 0, fake_push) == game::BarFill::NoHolder);
        CHECK(game::bar_fill(&bar.v, 5, 0, fake_push) == game::BarFill::NoHolder);
        CHECK(game::bar_fill(&bar.v, 1, 2, fake_push) == game::BarFill::NoHolder);
        CHECK(bar.pushes == 0);
    }
    SUBCASE("the placeholder is off") {
        ability_slots_resolve(false);
        FakeBar bar{57, 59, 54, 86, 102};
        g_fake = &bar;
        CHECK(game::bar_fill(&bar.v, 3, 0, fake_push) == game::BarFill::NoHolder);
        CHECK(bar.size() == 5);
    }
}

TEST_CASE("fourth-ability-slot: a bar that is not vanilla's is left alone, and logged once") {
    // covers: fourth-ability-slot/The fourth slot on the skill bar (the non-vanilla-bar case)
    BarRig rig;
    const uint64_t since = Log::get().sequence();
    FakeBar six{57, 59, 54, 86, 102, 115};   // another mod already pushed a sixth entry
    g_fake = &six;
    CHECK(game::bar_fill(&six.v, 1, 1, fake_push) == game::BarFill::NotVanillaBar);
    CHECK(six.size() == 6);
    FakeBar four{57, 59, 54, 86};
    g_fake = &four;
    CHECK(game::bar_fill(&four.v, 1, 1, fake_push) == game::BarFill::NotVanillaBar);
    FakeBar empty{};
    g_fake = &empty;
    CHECK(game::bar_fill(&empty.v, 1, 1, fake_push) == game::BarFill::NotVanillaBar);
    CHECK(six.pushes == 0);
    CHECK(four.pushes == 0);
    CHECK(empty.pushes == 0);
    int lines = 0;
    for (auto& e : Log::get().since(since))
        if (e.text.find("slot 4 is left alone") != std::string::npos) ++lines;
    CHECK(lines == 1);
}

TEST_CASE("fourth-ability-slot: the stat lookup answers for mod numbers only") {
    // covers: fourth-ability-slot/Smash unchanged (the unit-test half: every vanilla number gets no answer, so the detour calls the game)
    // covers: fourth-ability-slot/On cooldown, Not enough mp (the answers the game's own checks read)
    Loaded state;
    reset_registries();
    int32_t ms = -77;
    float mp = -1;
    // before the table is built the detours pass everything through
    CHECK(!game::stat_cooldown(112, -1, true, &ms));
    CHECK(!game::stat_cost(112, &mp));
    CHECK(!game::stat_duration(111, &ms));
    CHECK(ability_stat(111) == nullptr);

    BarRig rig;   // a:guard is 112 and a:wide is 113: good_def's values (800 ms, 0.25 mp, ranks 0 to 2: 6000, 5500, 5000)
    // every vanilla number 0..110 (smash is 54), and the numbers nobody defined: no answer, so ms, mp stay as they were
    for (uint32_t n : {0u, 1u, 54u, 109u, 110u, 114u, 200u, 255u, 256u, 1000u, 0xFFFFFFFFu}) {
        CHECK_MESSAGE(!game::stat_cooldown(n, -1, true, &ms), "number ", n);
        CHECK_MESSAGE(!game::stat_cost(n, &mp), "number ", n);
        CHECK_MESSAGE(!game::stat_duration(n, &ms), "number ", n);
    }
    CHECK(ms == -77);
    CHECK(mp == -1);
    CHECK(ability_stat(54) == nullptr);

    // the placeholder (design D8): no payload, so the values are the loader's own
    CHECK(game::stat_duration(111, &ms));
    CHECK(ms == 500);
    CHECK(game::stat_cost(111, &mp));
    CHECK(mp == 0);
    for (int32_t level : {-1, 0, 1, 7}) {
        CHECK(game::stat_cooldown(111, level, true, &ms));
        CHECK(ms == 2000);
    }

    CHECK(game::stat_duration(112, &ms));
    CHECK(ms == 800);
    CHECK(game::stat_cost(113, &mp));
    CHECK(mp == 0.25f);
}

TEST_CASE("fourth-ability-slot: the cooldown follows the rank, and level -1 is the creature's own rank") {
    // covers: fourth-ability-slot/On cooldown (the unit-test half)
    BarRig rig;
    int32_t ms = 0;
    // an explicit level is the rank, clamped to 0..max_rank (the tooltip asks for "next level" past the last one)
    CHECK(game::stat_cooldown(112, 0, true, &ms));
    CHECK(ms == 6000);
    CHECK(game::stat_cooldown(112, 1, true, &ms));
    CHECK(ms == 5500);
    CHECK(game::stat_cooldown(112, 2, false, &ms));
    CHECK(ms == 5000);
    CHECK(game::stat_cooldown(112, 3, true, &ms));
    CHECK(ms == 5000);
    CHECK(game::stat_cooldown(112, 500, true, &ms));
    CHECK(ms == 5000);
    CHECK(game::stat_cooldown(112, -2, false, &ms));   // below -1: also the creature's own rank
    CHECK(ms == 5500);                                  // another creature: rank 1

    // -1: the local player's rank comes from the rank source, any other creature has rank 1
    g_rank = 2;
    CHECK(game::stat_cooldown(112, -1, true, &ms));
    CHECK(ms == 5000);
    CHECK(game::stat_cooldown(112, -1, false, &ms));
    CHECK(ms == 5500);
    g_rank = 0;
    CHECK(game::stat_cooldown(112, -1, true, &ms));
    CHECK(ms == 6000);
    g_rank = 99;   // a rank above max_rank is clamped
    CHECK(game::stat_cooldown(112, -1, true, &ms));
    CHECK(ms == 5000);
}

TEST_CASE("fourth-ability-slot: the stat table is replaced when it is built again, and emptied with the callbacks") {
    BarRig rig;
    CHECK(ability_stat(112) != nullptr);
    CHECK(ability_stat(113) != nullptr);
    CHECK(ability_stat(114) == nullptr);
    ability_slots_resolve(false);   // placeholder off: the table still answers for 111, which a character cannot reach
    CHECK(ability_stat(111) != nullptr);
    ability_callbacks_clear();
    CHECK(ability_stat(112) == nullptr);
}

// ---- the step runner and the ends (task 4.4) ----

namespace {

using game::EndReason;

std::vector<std::string> g_calls;   // what the callbacks saw, as text
uint32_t g_fail_on_step = 0;        // the step (1-based) at which on_step reports a failure; 0: never
uint32_t g_fault_on_step = 0;       // the step at which on_step writes through a null pointer; 0: never
DWORD g_sleep_ms = 0;               // on_step sleeps this long

std::vector<std::string> g_markers;   // one entry per callback that was told a marker: "step 3 wind,hit", "end done", "start x"
uint32_t g_last_restarts = 0;         // ctx.restarts of the last step or watch step

// The names the native context carries, as "a,b".
std::string marker_text(const KyubuAbilityCtx* c) {
    std::string s;
    for (uint32_t i = 0; i < c->marker_count; ++i) s += (i ? "," : "") + std::string(c->markers[i]);
    return s;
}

void KYUBU_CALL rec_start(void*, const KyubuAbilityCtx* c) {
    g_calls.push_back("start " + std::string(c->id));
    if (c->marker_count) g_markers.push_back("start " + marker_text(c));
}
void KYUBU_CALL rec_step(void*, const KyubuAbilityCtx* c) {
    g_calls.push_back("step " + std::to_string(c->steps) + " " + std::to_string(c->elapsed_ms) + " " + std::to_string(c->dt_ms));
    g_last_restarts = c->restarts;
    if (c->marker_count) g_markers.push_back("step " + std::to_string(c->steps) + " " + marker_text(c));
    if (g_fail_on_step && c->steps == g_fail_on_step) game::ability_callback_failed();   // what the Lua trampoline does on an error
    if (g_fault_on_step && c->steps == g_fault_on_step) *(volatile int*)nullptr = 1;      // a native fault
    if (g_sleep_ms) Sleep(g_sleep_ms);
}
void KYUBU_CALL rec_end(void*, const KyubuAbilityCtx* c) {
    g_calls.push_back(std::string("end ") + game::end_reason_name((EndReason)c->reason));
    if (c->marker_count) g_markers.push_back(std::string("end ") + game::end_reason_name((EndReason)c->reason) + " " + marker_text(c));
}

// The watch callbacks (6.2): what a bystander's copy of another creature's run saw.
void KYUBU_CALL rec_watch_start(void*, const KyubuAbilityCtx* c) {
    g_calls.push_back("watch start " + std::string(c->id) + " rank " + std::to_string(c->rank) + (c->watch == 1 ? "" : " NOT WATCH") +
                      (c->side == KY_SIDE_CLIENT ? "" : " NOT CLIENT"));
}
void KYUBU_CALL rec_watch_step(void*, const KyubuAbilityCtx* c) {
    g_calls.push_back("watch step " + std::to_string(c->steps) + " " + std::to_string(c->elapsed_ms) + " " + std::to_string(c->dt_ms));
    g_last_restarts = c->restarts;
    if (c->marker_count) g_markers.push_back("watch step " + std::to_string(c->steps) + " " + marker_text(c));
    if (g_fail_on_step && c->steps == g_fail_on_step) game::ability_callback_failed();
}
void KYUBU_CALL rec_watch_end(void*, const KyubuAbilityCtx* c) {
    g_calls.push_back(std::string("watch end ") + game::end_reason_name((EndReason)c->reason));
}

template <typename T>
T* zeroed() { return (T*)VirtualAlloc(nullptr, sizeof(T), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); }

// A World with a local player, and the abilities a:guard (112, claims the warrior's specialization 1) and a:wide (113, claims
// the ranger) with callbacks: 800 ms, 0.25 mp. `watched`: a:guard also has watch callbacks (a:wide never has).
struct StepRig {
    Loaded state;
    cube_World* w = zeroed<cube_World>();
    cube_Creature* player = zeroed<cube_Creature>();
    cube_Creature* monster = zeroed<cube_Creature>();
    // `restarts`: a:guard's KyubuAbilityDef.restarts; it is then defined on the server side too (Server.exe's callbacks).
    explicit StepRig(bool watched = false, uint32_t restarts = 0) {
        g_calls.clear();
        g_markers.clear();
        g_last_restarts = 0;
        Animations::get().clear_bindings();
        g_fail_on_step = 0;
        g_fault_on_step = 0;
        g_sleep_ms = 0;
        game::reset_ability_step_for_test();
        Registries& R = reset_registries();
        DemoMod a("a");
        KyubuAbilityDef spec = claim_def(1, 1), any = claim_def(2, -1);
        spec.on_start = any.on_start = rec_start;
        spec.on_step = any.on_step = rec_step;
        spec.on_end = any.on_end = rec_end;
        if (watched) {
            spec.watch_start = rec_watch_start;
            spec.watch_step = rec_watch_step;
            spec.watch_end = rec_watch_end;
        }
        spec.restarts = restarts;
        REQUIRE(api_table()->ability_define(a.on(KY_SIDE_CLIENT), "a:guard", &spec) == KY_OK);
        if (restarts) REQUIRE(api_table()->ability_define(a.on(KY_SIDE_SERVER), "a:guard", &spec) == KY_OK);
        REQUIRE(api_table()->ability_define(a.on(KY_SIDE_CLIENT), "a:wide", &any) == KY_OK);
        R.freeze(by_name);
        ability_slots_resolve(true);
        w->local_player = player;
        player->data.hp = 100;
        player->data.mp = 1.0f;
        monster->data.hp = 100;
        monster->data.hostile_type = 1;
    }
    ~StepRig() {
        Animations::get().clear_bindings();
        game::reset_ability_step_for_test();
        reset_registries();
        VirtualFree(w, 0, MEM_RELEASE);
        VirtualFree(player, 0, MEM_RELEASE);
        VirtualFree(monster, 0, MEM_RELEASE);
    }
    // One World step with the player's mode time at `ms`: the hook at the site, then the pass.
    void step(uint32_t ms, uint32_t mode = 112) {
        player->data.current_mode = (uint8_t)mode;
        player->data.mode_start_time = ms;
        game::ability_step_creature(w, player);
        if (player->data.current_mode == mode) player->data.mode_start_time = ms + 16;   // the game's own time step, after the site
        game::ability_end_pass();
    }
    int count(const std::string& what) const {
        int n = 0;
        for (auto& c : g_calls) n += c.rfind(what, 0) == 0;
        return n;
    }
};

}  // namespace

TEST_CASE("fourth-ability-slot: the end reasons, in order") {
    // covers: fourth-ability-slot/Ends on time, Stunned (the unit-test half: the reason logic)
    game::EndFacts f;
    f.number = 112;
    f.mode = 112;
    f.duration_ms = 800;
    f.hp = 50;
    f.elapsed_ms = 400;
    CHECK(game::decide_end(f).reason == EndReason::None);   // running
    f.elapsed_ms = 799;
    CHECK(game::decide_end(f).reason == EndReason::None);
    f.elapsed_ms = 800;   // at the duration, and the mode is still the number: Kyubu ends it (the game would wait 10 s)
    CHECK(game::decide_end(f).reason == EndReason::Done);
    CHECK(game::decide_end(f).write_mode_zero);

    f.mode = 0;   // the mode is 0: done when the last time reached the duration, else the game cut it short
    f.elapsed_ms = 810;
    CHECK(game::decide_end(f).reason == EndReason::Done);
    CHECK_FALSE(game::decide_end(f).write_mode_zero);
    f.elapsed_ms = 300;
    CHECK(game::decide_end(f).reason == EndReason::Interrupted);
    f.report_slack_ms = game::kReportEndSlackMs;   // a remote player on Server.exe: its last report trails its own end
    f.elapsed_ms = 800 - game::kReportEndSlackMs;
    CHECK(game::decide_end(f).reason == EndReason::Done);
    f.elapsed_ms = 800 - game::kReportEndSlackMs - 1;
    CHECK(game::decide_end(f).reason == EndReason::Interrupted);
    f.report_slack_ms = 0;
    f.mode = 57;   // another ability took over
    CHECK(game::decide_end(f).reason == EndReason::Interrupted);
    CHECK_FALSE(game::decide_end(f).write_mode_zero);

    f.mode = 112;
    f.elapsed_ms = 100;
    f.stun_time = 1500;
    CHECK(game::decide_end(f).reason == EndReason::Interrupted);   // the stun field is not known
    CHECK(game::decide_end(f).write_mode_zero);
    f.stun_known = true;
    CHECK(game::decide_end(f).reason == EndReason::Stunned);
    f.cancel_marked = true;   // cancelled comes before a stun, died before cancelled, and gone before all
    CHECK(game::decide_end(f).reason == EndReason::Cancelled);
    f.hp = 0;
    CHECK(game::decide_end(f).reason == EndReason::Died);
    f.hp = -3;
    CHECK(game::decide_end(f).reason == EndReason::Died);
    f.gone = true;
    CHECK(game::decide_end(f).reason == EndReason::Gone);
    CHECK_FALSE(game::decide_end(f).write_mode_zero);   // there is no creature to write to

    CHECK(game::mp_after_cost(1.0f, 0.25f) == 0.75f);
    CHECK(game::mp_after_cost(0.1f, 0.25f) == 0.0f);   // never below 0
    CHECK(game::mp_after_cost(0.5f, 0.0f) == 0.5f);

    using game::StepKind;
    CHECK(game::classify_step(false, 0, 0, 112, 5) == StepKind::Start);
    CHECK(game::classify_step(true, 112, 16, 112, 32) == StepKind::Continue);
    CHECK(game::classify_step(true, 112, 400, 112, 16) == StepKind::Replace);   // the time went back: a new press
    CHECK(game::classify_step(true, 112, 16, 113, 32) == StepKind::Replace);    // another number
    // a watched run (6.2): a step back of up to the given ms is the same run, held; more is a new start
    CHECK(game::classify_step(true, 112, 400, 112, 370, game::kWatchStepBackMs) == StepKind::Hold);
    CHECK(game::classify_step(true, 112, 400, 112, 300, game::kWatchStepBackMs) == StepKind::Hold);
    CHECK(game::classify_step(true, 112, 400, 112, 299, game::kWatchStepBackMs) == StepKind::Replace);
    CHECK(game::classify_step(true, 112, 400, 112, 0, game::kWatchStepBackMs) == StepKind::Replace);   // a Fire Mage release
    CHECK(game::classify_step(true, 112, 400, 113, 450, game::kWatchStepBackMs) == StepKind::Replace);
    CHECK(game::classify_step(true, 112, 400, 112, 450, game::kWatchStepBackMs) == StepKind::Continue);
}

TEST_CASE("bystanders: a watched run ends only when the mode leaves the number") {
    // covers: add-skills-unleashed-multiplayer 6.2 (decide_watch_end)
    game::EndFacts f;
    f.number = 112;
    f.mode = 112;
    f.duration_ms = 800;
    f.elapsed_ms = 900;   // past the duration, still in the mode: the server ends it, not the bystander
    CHECK(game::decide_watch_end(f).reason == EndReason::None);
    f.hp = 0;
    f.cancel_marked = true;
    f.stun_time = 500;
    CHECK(game::decide_watch_end(f).reason == EndReason::None);   // nothing ends it while the mode is the number
    f.mode = 0;
    CHECK(game::decide_watch_end(f).reason == EndReason::Cancelled);
    CHECK_FALSE(game::decide_watch_end(f).write_mode_zero);
    f.cancel_marked = false;
    CHECK(game::decide_watch_end(f).reason == EndReason::Died);
    f.hp = 1;
    CHECK(game::decide_watch_end(f).reason == EndReason::Interrupted);   // the stun field is not known
    f.stun_known = true;
    CHECK(game::decide_watch_end(f).reason == EndReason::Stunned);
    f.stun_time = 0;
    CHECK(game::decide_watch_end(f).reason == EndReason::Done);
    f.report_slack_ms = game::kReportEndSlackMs;   // the bystander's last time trails the owner's end
    f.elapsed_ms = 800 - game::kReportEndSlackMs;
    CHECK(game::decide_watch_end(f).reason == EndReason::Done);
    f.elapsed_ms = 800 - game::kReportEndSlackMs - 1;
    CHECK(game::decide_watch_end(f).reason == EndReason::Interrupted);
    f.elapsed_ms = 900;
    f.mode = 54;   // another mode
    CHECK(game::decide_watch_end(f).reason == EndReason::Interrupted);
    f.gone = true;
    CHECK(game::decide_watch_end(f).reason == EndReason::Gone);
    for (bool g : {false, true}) {
        f.gone = g;
        CHECK_FALSE(game::decide_watch_end(f).write_mode_zero);
    }
}

TEST_CASE("fourth-ability-slot: steps counted, cost taken once, ends on time") {
    // covers: fourth-ability-slot/Steps counted, Cost taken once, Ends on time (the unit-test half)
    StepRig r;
    // a:guard is 112: 800 ms, mp 0.25. The mode time rises by 16 ms a step.
    r.step(0);
    CHECK(r.player->data.mp == 0.75f);
    CHECK(r.count("start a:guard") == 1);
    CHECK(g_calls[1] == "step 1 0 0");
    for (uint32_t ms = 16; ms < 784; ms += 16) r.step(ms);
    CHECK(r.player->data.mp == 0.75f);   // the cost is taken once, not per step
    CHECK(r.count("start") == 1);
    CHECK(r.count("step") == 49);        // 0, 16, ..., 768
    CHECK(r.count("end") == 0);
    CHECK(g_calls[2] == "step 2 16 16");
    CHECK(game::ability_active_count() == 1);

    // the step whose hook reads 784: the game moves the time on to 800 after the site, so the pass finds the duration reached;
    // on_step ran for it, and on_end follows with the time the ability ended at
    r.step(784);
    CHECK(r.count("step") == 50);
    CHECK(r.count("end done") == 1);
    CHECK(g_calls.back() == "end done");
    CHECK(r.player->data.current_mode == 0);   // Kyubu wrote the end: the game would wait for its own 10 s rule
    CHECK(r.player->data.mode_start_time == 0);
    CHECK(game::ability_active_count() == 0);
    r.step(816, 0);   // mode 0: nothing runs, nothing ends again
    CHECK(r.count("end") == 1);
}

namespace {

// A clip of the ability's own length (a:guard, 800 ms): a marker at the start, one at a quarter (200 ms), one at the half (400 ms) and
// one at the very end (800 ms). Written out of order on purpose: the order told is the order crossed.
const char* kMarkedClip =
    "duration = \"ability\"\n\n"
    "[[marker]]\nt = 1.0\nname = \"done\"\n\n"
    "[[marker]]\nt = 0.5\nname = \"hit\"\n\n"
    "[[marker]]\nt = 0.0\nname = \"start\"\n\n"
    "[[marker]]\nt = 0.25\nname = \"wind\"\n";

void bind_marked_clip(const char* ability = "a:guard") {
    std::string errors;
    REQUIRE_MESSAGE(Animations::get().define_from_text("a", "swing", kMarkedClip, &errors), errors);
    Animations::get().bind(ability, "a:swing");
}

}  // namespace

TEST_CASE("ability-animation: markers are told to on_step once, from the ability's mode time") {
    // covers: ability-animation/Markers: Between two steps; Markers are read by the ability: From the mode time (the unit-test half)
    StepRig r;
    bind_marked_clip();
    r.step(0);   // the first step covers the time from the start: a marker at t = 0 is told here, and not to on_start
    CHECK(r.count("start a:guard") == 1);
    REQUIRE(g_markers.size() == 1);
    CHECK(g_markers[0] == "step 1 start");
    for (uint32_t ms = 16; ms < 784; ms += 16) r.step(ms);
    // the 200 ms marker falls in (192, 208] and the 400 ms marker in (384, 400]: a step covers its start exclusive, its end inclusive
    REQUIRE(g_markers.size() == 3);
    CHECK(g_markers[1] == "step 14 wind");   // steps are numbered from 1: the step at 208 ms is the 14th
    CHECK(g_markers[2] == "step 26 hit");    // 400 ms is the 26th
    // the step whose hook reads 784 is the last: the time moves on to 800 after the site and the ability ends; its on_end is told the
    // marker at the clip's end, which no step could have reached, and it is told once
    r.step(784);
    REQUIRE(g_markers.size() == 4);
    CHECK(g_markers[3] == "end done done");
    CHECK(r.count("end done") == 1);
    r.step(816, 0);
    CHECK(g_markers.size() == 4);   // nothing is told again after the end
}

TEST_CASE("ability-animation: a long step tells the markers in the order crossed, and a restart starts over") {
    // covers: ability-animation/Markers: Order crossed (through the step runner), and the start of a new activation
    StepRig r;
    bind_marked_clip();
    r.step(0);
    CHECK(g_markers.back() == "step 1 start");
    r.step(500);   // one step over the first 500 ms: wind (200) then hit (400), whatever order the file wrote them in
    CHECK(g_markers.back() == "step 2 wind,hit");
    r.step(520);
    CHECK(g_markers.size() == 2);   // nothing in (500, 520]
    r.step(1 << 4, 112);   // the mode time went back: the game started the ability again, a new activation
    CHECK(r.count("end interrupted") == 1);
    CHECK(g_markers.back() == "step 1 start");   // starts over from t = 0, and the marker at 0 is told once more
    CHECK(g_markers.size() == 3);
}

TEST_CASE("ability-animation: no clip, no markers, and a rebound ability tells the new clip's") {
    // covers: ability-animation/Markers (the fallbacks: an ability with no clip, or a clip with none, tells nothing)
    StepRig r;
    r.step(0);
    r.step(400);
    CHECK(g_markers.empty());   // a:guard has no clip
    std::string errors;
    REQUIRE_MESSAGE(Animations::get().define_from_text("a", "plain", "duration = 400\n", &errors), errors);
    Animations::get().bind("a:guard", "a:plain");
    r.step(416);
    CHECK(g_markers.empty());   // a clip with no marker
    bind_marked_clip();
    r.step(432);   // (416, 432] holds no marker of the new clip
    r.step(800 - 16);
    REQUIRE(g_markers.size() == 1);
    CHECK(g_markers[0] == "end done done");   // the span from the last step to the end holds the marker at 800
}

TEST_CASE("fourth-ability-slot: only a registered number, and only the creatures the game's guard runs") {
    StepRig r;
    for (uint32_t mode : {0u, 1u, 54u, 110u, 114u, 200u}) {   // vanilla, none, and numbers nobody defined
        r.player->data.current_mode = (uint8_t)mode;
        game::ability_step_creature(r.w, r.player);
    }
    CHECK(g_calls.empty());
    CHECK(game::ability_active_count() == 0);

    // a monster in a joined client's World belongs to the server: the game runs no mode block for it, and neither does Kyubu
    r.w->joined = 1;
    r.monster->data.current_mode = 112;
    game::ability_step_creature(r.w, r.monster);
    CHECK(g_calls.empty());
    CHECK(game::ability_active_count() == 0);
    r.w->joined = 0;   // single-player: a monster's mode runs on the server side, where a:guard has no callbacks
    game::ability_step_creature(r.w, r.monster);
    CHECK(game::ability_active_count() == 1);
    CHECK(g_calls.empty());
    // a player that is not the local player (another client's, hostile_type 0) is not run in single-player either
    cube_Creature* other = zeroed<cube_Creature>();
    other->data.current_mode = 112;
    game::ability_step_creature(r.w, other);
    CHECK(game::ability_active_count() == 1);
    VirtualFree(other, 0, MEM_RELEASE);
    game::ability_end_all(EndReason::Gone);
}

TEST_CASE("fourth-ability-slot: on Server.exe a remote player's mod mode runs, with the rank its client sent") {
    // covers: add-skills-unleashed-multiplayer 3.2 (the server's guard, rank and cost)
    StepRig r;
    const auto saved = L().host.host;
    L().host.host = KY_HOST_SERVER;
    // the server's World has no local player and a client's player has hostile_type 0: the client-side guard would skip it
    r.w->local_player = nullptr;
    cube_Creature* remote = zeroed<cube_Creature>();
    remote->data.hp = 100;
    remote->data.mp = 1.0f;
    remote->data.current_mode = 112;
    remote->data.skills[9] = 7;   // above a:guard's max_rank (2)
    CHECK(game::ability_runs_here(r.w, remote));
    CHECK(game::ability_rank_for(r.w, remote, 112) == 2);
    remote->data.skills[9] = 1;
    CHECK(game::ability_rank_for(r.w, remote, 112) == 1);
    remote->data.skills[9] = 0;
    CHECK(game::ability_rank_for(r.w, remote, 112) == 0);   // an unlearnt rank is reported as it is; the start refuses it (3.3)
    remote->data.skills[9] = 1;
    game::ability_step_creature(r.w, remote);
    CHECK(game::ability_active_count() == 1);
    CHECK(remote->data.mp == doctest::Approx(0.75f));   // the server takes the cost the game does not take for a mod number
    // the same creature on a client is not run: it is not the local player and the World is joined
    L().host.host = KY_HOST_CLIENT;
    r.w->joined = 1;
    CHECK(!game::ability_runs_here(r.w, remote));
    L().host.host = saved;
    VirtualFree(remote, 0, MEM_RELEASE);
    game::ability_end_all(EndReason::Gone);
}

TEST_CASE("bystanders: a joined client watches another player's mod mode with the watch callbacks only") {
    // covers: add-skills-unleashed-multiplayer 6.2 (the bystander path)
    StepRig r(true);
    r.w->joined = 1;
    cube_Creature* remote = zeroed<cube_Creature>();
    remote->data.hp = 100;
    remote->data.mp = 1.0f;
    remote->data.skills[9] = 7;   // above a:guard's max_rank (2)
    auto step = [&](uint32_t ms, uint32_t mode = 112) {   // a World step of the bystander; the time is the server's
        remote->data.current_mode = (uint8_t)mode;
        remote->data.mode_start_time = ms;
        game::ability_step_creature(r.w, remote);
        game::ability_end_pass();
    };
    CHECK(game::ability_watch_here(r.w, remote));
    CHECK_FALSE(game::ability_watch_here(r.w, r.player));
    CHECK(game::ability_watch_rank(remote, 112) == 2);
    remote->data.skills[9] = 0;
    CHECK(game::ability_watch_rank(remote, 112) == 1);   // an unknown rank reads 1
    remote->data.skills[9] = 2;

    step(0);
    REQUIRE(g_calls.size() == 2);
    CHECK(g_calls[0] == "watch start a:guard rank 2");
    CHECK(g_calls[1] == "watch step 1 0 0");
    CHECK(remote->data.mp == 1.0f);   // no cost: the mp is the server's
    step(50);
    CHECK(g_calls.back() == "watch step 2 50 50");
    step(30);   // a server update set the time back: held at 50
    CHECK(g_calls.back() == "watch step 3 50 0");
    step(80);
    CHECK(g_calls.back() == "watch step 4 80 30");
    CHECK(r.count("start") == 0);   // the creature's own callbacks never run here
    CHECK(r.count("step") == 0);

    // a time far back (more than kWatchStepBackMs) is a new run of the same ability
    step(300);
    CHECK(g_calls.back() == "watch step 5 300 220");
    step(0);
    CHECK(r.count("watch end interrupted") == 1);
    CHECK(r.count("watch start") == 2);
    CHECK(g_calls.back() == "watch step 1 0 0");

    // past the duration the mode is still the server's: no end, and the mode is not written
    step(900);
    CHECK(r.count("watch end") == 1);
    CHECK(remote->data.current_mode == 112);
    CHECK(remote->data.mode_start_time == 900);
    step(916, 0);   // the server's update ends it
    CHECK(g_calls.back() == "watch end done");
    CHECK(game::ability_active_count() == 0);

    // a failed watch callback: no more steps, the end waits for the mode to leave
    g_fail_on_step = 2;
    step(0);
    step(20);
    step(40);
    CHECK(g_calls.back() == "watch step 2 20 20");
    CHECK(game::ability_active_count() == 1);
    step(60, 0);
    CHECK(g_calls.back() == "watch end cancelled");
    g_fail_on_step = 0;

    // the local player's own run is unchanged, and a:wide (no watch callbacks) is not watched
    r.step(0);
    CHECK(r.count("start a:guard") == 1);
    const size_t before = g_calls.size();
    step(0, 113);
    CHECK(g_calls.size() == before);
    CHECK(game::ability_active_count() == 1);   // the local player's only
    game::ability_end_all(EndReason::Gone);
    CHECK(r.count("watch end") == 3);   // ability_end_all ended only the local player's

    // single-player and Server.exe watch nobody
    r.w->joined = 0;
    CHECK_FALSE(game::ability_watch_here(r.w, remote));
    const auto saved = L().host.host;
    L().host.host = KY_HOST_SERVER;
    r.w->joined = 1;
    CHECK_FALSE(game::ability_watch_here(r.w, remote));
    L().host.host = saved;
    VirtualFree(remote, 0, MEM_RELEASE);
}

TEST_CASE("bystanders: on Server.exe a remote player's reported time may step back, and its end trails the duration") {
    // covers: add-skills-unleashed-multiplayer 6.2 (watch-berserker-0720 run 3: a back step at 203 ms was taken for a new start,
    // refused for the cooldown, and the refusal cut the run on every client; run 2: `interrupted` at 1591 of 1700 ms)
    StepRig r;
    const auto saved = L().host.host;
    L().host.host = KY_HOST_SERVER;
    r.w->local_player = nullptr;
    game::set_server_clock_for_test(10000);
    cube_Creature* remote = zeroed<cube_Creature>();
    remote->data.hp = 100;
    remote->data.mp = 1.0f;
    remote->data.skills[9] = 1;
    auto update = [&](uint32_t mode, uint32_t ms) {
        remote->data.current_mode = (uint8_t)mode;
        remote->data.mode_start_time = ms;
        game::ability_step_creature(r.w, remote);
        game::ability_end_pass();
    };
    const uint64_t seq = Log::get().sequence();
    update(112, 0);
    REQUIRE(game::ability_active_count() == 1);
    update(112, 200);
    update(112, 180);   // an update set the time back 20 ms: the same run, not a refused restart
    CHECK(remote->data.current_mode == 112);
    CHECK(game::ability_active_count() == 1);
    CHECK_FALSE(logged(seq, "refuses"));
    CHECK(remote->data.mp == doctest::Approx(0.75f));   // the cost was taken once
    update(112, 800 - game::kReportEndSlackMs);
    update(0, 0);       // the client ended it at the duration; its last report trailed that
    CHECK(game::ability_active_count() == 0);
    CHECK(logged(seq, "ends `done`"));
    CHECK_FALSE(logged(seq, "ends `interrupted`"));
    L().host.host = saved;
    VirtualFree(remote, 0, MEM_RELEASE);
}

TEST_CASE("bystanders: a restart is a reported run's time set back to about 0, within the count") {
    // covers: add-skills-unleashed-multiplayer 8.1 (KyubuAbilityDef.restarts, is_restart)
    using game::is_restart;
    CHECK(is_restart(true, 112, 1500, 112, 0, 0, 1));
    CHECK(is_restart(true, 112, 1500, 112, game::kRestartWindowMs, 0, 1));
    CHECK_FALSE(is_restart(true, 112, 1500, 112, game::kRestartWindowMs + 1, 0, 1));   // too far in: a new start
    CHECK_FALSE(is_restart(true, 112, 1500, 112, 0, 1, 1));    // the count is used up
    CHECK_FALSE(is_restart(true, 112, 1500, 112, 0, 0, 0));    // the ability allows none
    CHECK_FALSE(is_restart(false, 112, 1500, 112, 0, 0, 1));   // the creature's own run
    CHECK_FALSE(is_restart(true, 112, 1500, 113, 0, 0, 1));    // another number
    CHECK_FALSE(is_restart(true, 112, 90, 112, 0, 0, 1));      // a back step of 100 ms or less is a Hold
    CHECK(is_restart(true, 112, 101, 112, 0, 0, 1));
    CHECK_FALSE(is_restart(true, 112, 200, 112, 300, 0, 1));   // forward
}

TEST_CASE("bystanders: on Server.exe a remote player's restart goes on as the same run, once") {
    // covers: add-skills-unleashed-multiplayer 8.1 (a Fire Mage release sets the client's mode time to 0 after the charge; taken
    // for a new start, the server refused it for the cooldown and took the mode back)
    StepRig r(false, 1);
    const auto saved = L().host.host;
    L().host.host = KY_HOST_SERVER;
    r.w->local_player = nullptr;
    game::set_server_clock_for_test(10000);
    cube_Creature* remote = zeroed<cube_Creature>();
    remote->data.hp = 100;
    remote->data.mp = 1.0f;
    remote->data.skills[9] = 1;
    auto update = [&](uint32_t mode, uint32_t ms) {
        remote->data.current_mode = (uint8_t)mode;
        remote->data.mode_start_time = ms;
        game::ability_step_creature(r.w, remote);
        game::ability_end_pass();
    };
    const uint64_t seq = Log::get().sequence();
    update(112, 0);
    update(112, 300);
    update(112, 600);
    update(112, 20);    // the release: the same run, restarted
    CHECK(remote->data.current_mode == 112);
    CHECK(game::ability_active_count() == 1);
    CHECK(r.count("start a:guard") == 1);
    CHECK(r.count("end") == 0);
    CHECK(g_calls.back() == "step 4 20 0");   // dt 0 at the restart
    CHECK(g_last_restarts == 1);
    CHECK(logged(seq, "restarts (1 of 1)"));
    CHECK_FALSE(logged(seq, "refuses"));
    CHECK(remote->data.mp == doctest::Approx(0.75f));   // the cost once
    update(112, 60);
    CHECK(g_calls.back() == "step 5 60 40");
    CHECK(g_last_restarts == 1);
    update(112, 400);
    update(112, 10);    // a second one is past the count: a new start, refused for the cooldown
    CHECK(r.count("end interrupted") == 1);
    CHECK(logged(seq, "refuses a:guard"));
    CHECK(remote->data.current_mode == 0);
    L().host.host = saved;
    VirtualFree(remote, 0, MEM_RELEASE);
}

TEST_CASE("bystanders: on Server.exe a cooldown given back admits the next start at once") {
    // covers: add-skills-unleashed-multiplayer 8.1 (a fizzled Fire Mage charge: the client clears its cooldown, and the server
    // module gives the admission's back through ability_cooldown_set)
    StepRig r(false, 1);
    const auto saved = L().host.host;
    L().host.host = KY_HOST_SERVER;
    r.w->local_player = nullptr;
    game::set_server_clock_for_test(10000);
    cube_Creature* remote = zeroed<cube_Creature>();
    remote->data.hp = 100;
    remote->data.mp = 1.0f;
    remote->data.skills[9] = 1;
    auto update = [&](uint32_t mode, uint32_t ms) {
        remote->data.current_mode = (uint8_t)mode;
        remote->data.mode_start_time = ms;
        game::ability_step_creature(r.w, remote);
        game::ability_end_pass();
    };
    const KyubuCreature h = game::handle_for(KY_SIDE_SERVER, remote);
    update(112, 0);
    update(112, 300);
    update(0, 0);   // the client's run ended without a cast
    CHECK(r.count("start a:guard") == 1);
    game::set_server_clock_for_test(10500);
    remote->data.mp = 1.0f;
    update(112, 0);   // the cooldown the admission started: refused
    CHECK(remote->data.current_mode == 0);
    CHECK(r.count("start a:guard") == 1);

    game::set_server_clock_for_test(10500 + 1800 + 1);   // past the refused run
    game::ability_server_cooldown_set(h, 112, 0);
    update(112, 0);
    CHECK(remote->data.current_mode == 112);
    CHECK(r.count("start a:guard") == 2);
    update(0, 0);

    // a time set instead holds the next start until it is over
    game::ability_server_cooldown_set(h, 112, 2000);
    game::set_server_clock_for_test(10500 + 1800 + 1 + 1999);
    update(112, 0);
    CHECK(remote->data.current_mode == 0);
    CHECK(r.count("start a:guard") == 2);

    // off Server.exe it does nothing
    L().host.host = saved;
    game::ability_server_cooldown_set(h, 112, 0);
    VirtualFree(remote, 0, MEM_RELEASE);
}

TEST_CASE("bystanders: a watched restart tells the markers again, from the creature's own clip") {
    // covers: add-skills-unleashed-multiplayer 8.1 (ability_animation_bind_for: a Fire Mage's release clip for one creature)
    StepRig r(true, 1);
    r.w->joined = 1;
    cube_Creature* remote = zeroed<cube_Creature>();
    remote->data.hp = 100;
    remote->data.skills[9] = 1;
    remote->id = 77;
    std::string errors;
    REQUIRE_MESSAGE(Animations::get().define_from_text("a", "charge", "duration = 800\n", &errors), errors);
    REQUIRE_MESSAGE(Animations::get().define_from_text("a", "release", "duration = 400\n\n[[marker]]\nt = 0.5\nname = \"cast\"\n",
                                                       &errors), errors);
    Animations::get().bind("a:guard", "a:charge");
    auto step = [&](uint32_t ms) {
        remote->data.current_mode = 112;
        remote->data.mode_start_time = ms;
        game::ability_step_creature(r.w, remote);
        game::ability_end_pass();
    };
    step(0);
    step(500);
    CHECK(g_markers.empty());   // the charge clip has none
    Animations::get().bind_for("a:guard", 77, "a:release");   // what the watch callback does when it sees the restart
    step(0);
    CHECK(g_last_restarts == 1);
    CHECK(r.count("watch start") == 1);
    step(150);
    CHECK(g_markers.empty());
    step(250);   // the release clip's marker at 200 ms
    REQUIRE(g_markers.size() == 1);
    CHECK(g_markers[0] == "watch step 5 cast");
    // the local player keeps the ability's own clip
    std::string clip;
    REQUIRE(Animations::get().bound_clip("a:guard", 5, &clip));
    CHECK(clip == "a:charge");
    VirtualFree(remote, 0, MEM_RELEASE);
}

TEST_CASE("bystanders: the creature's own run is not restarted, whatever the count") {
    // covers: add-skills-unleashed-multiplayer 8.1 (single-player and the local player: the mod sees an end and a start, as before)
    StepRig r(false, 1);
    r.step(0);
    r.step(600);
    r.step(0);
    CHECK(r.count("end interrupted") == 1);
    CHECK(r.count("start a:guard") == 2);
    CHECK(g_last_restarts == 0);
}

TEST_CASE("fourth-ability-slot: the server's admission, rule by rule") {
    // covers: add-skills-unleashed-multiplayer 3.3 (N1, T3 to T5: rank, cooldown, mp)
    using game::ServerRefusal;
    CHECK(game::server_admit(1, 1.0f, 0, false, 0.25f, 1000, 0) == ServerRefusal::None);
    CHECK(game::server_admit(0, 1.0f, 0, false, 0.25f, 1000, 0) == ServerRefusal::Rank);       // rank first
    CHECK(game::server_admit(0, 0.0f, 0, false, 0.25f, 1000, 5000) == ServerRefusal::Rank);
    CHECK(game::server_admit(2, 0.0f, 0, false, 0.25f, 1000, 5000) == ServerRefusal::Cooldown);   // then the cooldown
    CHECK(game::server_admit(2, 1.0f, 0, false, 0.25f, 4999, 5000) == ServerRefusal::Cooldown);
    CHECK(game::server_admit(2, 1.0f, 0, false, 0.25f, 5000, 5000) == ServerRefusal::None);       // ready at ready_at
    CHECK(game::server_admit(1, 0.2f, 0, false, 0.25f, 1000, 0) == ServerRefusal::Mp);
    CHECK(game::server_admit(1, 0.25f, 0, false, 0.25f, 1000, 0) == ServerRefusal::None);         // exactly enough
    // the update with the start may carry the mp with the cost already off: the value from before the start counts too
    CHECK(game::server_admit(1, 0.0f, 0.25f, true, 0.25f, 1000, 0) == ServerRefusal::None);
    CHECK(game::server_admit(1, 0.1f, 0.2f, true, 0.25f, 1000, 0) == ServerRefusal::Mp);
    CHECK(game::server_admit(1, 0.0f, 0.0f, true, 0.0f, 1000, 0) == ServerRefusal::None);           // no cost, no mp needed
    CHECK(std::string(game::server_refusal_name(ServerRefusal::Rank)) == "rank 0");
}

TEST_CASE("fourth-ability-slot: on Server.exe a remote player's start is refused for rank 0, cooldown and mp") {
    // covers: add-skills-unleashed-multiplayer 3.3 (the refusal takes the mode back in the server's copy; nothing runs)
    StepRig r;
    const auto saved = L().host.host;
    L().host.host = KY_HOST_SERVER;
    r.w->local_player = nullptr;
    game::set_server_clock_for_test(10000);
    cube_Creature* remote = zeroed<cube_Creature>();
    remote->data.hp = 100;
    remote->data.mp = 1.0f;
    auto update = [&](uint32_t mode, uint32_t ms) {   // what the client's creature update leaves in the server's copy, then a step
        remote->data.current_mode = (uint8_t)mode;
        remote->data.mode_start_time = ms;
        game::ability_step_creature(r.w, remote);
        game::ability_end_pass();
    };
    // rank 0: refused, the mode taken back, nothing runs, and the next updates of the same run are refused too
    remote->data.skills[9] = 0;
    update(112, 0);
    CHECK(remote->data.current_mode == 0);
    CHECK(game::ability_active_count() == 0);
    update(112, 40);
    update(112, 80);
    CHECK(remote->data.current_mode == 0);
    CHECK(game::ability_active_count() == 0);
    CHECK(remote->data.mp == doctest::Approx(1.0f));   // a refused start costs nothing

    // rank 1: accepted; the cooldown (5500 ms at rank 1) starts on the server's clock
    remote->data.skills[9] = 1;
    game::set_server_clock_for_test(20000);   // the refused run is over
    update(112, 0);
    CHECK(game::ability_active_count() == 1);
    CHECK(remote->data.mp == doctest::Approx(0.75f));
    update(0, 0);   // the client's run ended
    CHECK(game::ability_active_count() == 0);
    g_calls.clear();
    game::set_server_clock_for_test(20000 + 5499);
    update(112, 0);
    CHECK(remote->data.current_mode == 0);   // 1 ms of cooldown left
    CHECK(game::ability_active_count() == 0);
    game::set_server_clock_for_test(20000 + 5500 + 1800 + 1);   // past the refused run, and the cooldown is over
    update(112, 0);
    CHECK(game::ability_active_count() == 1);
    update(0, 0);

    // mp: the server's after-step pass reads the players of the World's map (one node here: the remote player)
    struct Node {
        std_tree_node n;
        uint64_t key;
        cube_Creature* value;
    } head = {}, root = {};
    head.n.isnil = 1;
    head.n.parent = head.n.left = head.n.right = &root.n;
    root.n.parent = root.n.left = root.n.right = &head.n;
    root.key = 7;
    root.value = remote;
    r.w->creatures.head = &head.n;
    r.w->creatures.size = 1;
    // the server held 0.2 before the start and the update with it reports 0.1: refused (0.25 needed)
    game::set_server_clock_for_test(40000);
    remote->data.current_mode = 0;
    remote->data.mp = 0.2f;
    game::ability_server_after_step(r.w);
    remote->data.mp = 0.1f;
    g_calls.clear();
    update(112, 0);
    CHECK(remote->data.current_mode == 0);
    CHECK(game::ability_active_count() == 0);
    // the server held 0.25 and the update reports 0 (the client took the cost before it sent): accepted, the cost taken once
    game::set_server_clock_for_test(50000);
    remote->data.mp = 0.25f;
    game::ability_server_after_step(r.w);
    remote->data.mp = 0.0f;
    update(112, 0);
    CHECK(game::ability_active_count() == 1);
    CHECK(remote->data.mp == doctest::Approx(0.0f));
    // while it is in the mod mode the pass keeps the value from before the start
    remote->data.mp = 0.9f;
    game::ability_server_after_step(r.w);
    update(0, 0);
    remote->data.mp = 0.0f;
    game::set_server_clock_for_test(70000);
    update(112, 0);   // the value kept was 0.25 (from before the last start), not 0.9: enough
    CHECK(game::ability_active_count() == 1);
    update(0, 0);
    r.w->creatures.head = nullptr;
    r.w->creatures.size = 0;

    game::set_server_clock_for_test(-1);
    L().host.host = saved;
    VirtualFree(remote, 0, MEM_RELEASE);
    game::ability_end_all(EndReason::Gone);
}

TEST_CASE("fourth-ability-slot: a stun ends the ability, and on_step does not run again") {
    // covers: fourth-ability-slot/Stunned (the unit-test half)
    for (int known : {1, 0}) {
        StepRig r;
        game::set_stun_known_for_test(known);
        r.step(0);
        r.step(16);
        r.player->data.stun_time = 1500;
        r.player->data.current_mode = 0;   // the game clears the mode in the step that finds the stun
        r.player->data.mode_start_time = 0;
        game::ability_end_pass();
        CHECK(g_calls.back() == (known ? "end stunned" : "end interrupted"));
        CHECK(r.count("end") == 1);
        const int steps = r.count("step");
        game::ability_end_pass();   // the record is gone: neither on_step nor a second on_end follows
        CHECK(r.count("step") == steps);
        CHECK(r.count("end") == 1);
    }
}

TEST_CASE("fourth-ability-slot: death, interruption and another number") {
    StepRig r;
    r.step(0);
    r.step(16);
    r.player->data.current_mode = 57;   // a dodge roll or another ability replaced it
    game::ability_end_pass();
    CHECK(g_calls.back() == "end interrupted");
    CHECK(r.player->data.current_mode == 57);   // the game's mode is left alone

    g_calls.clear();
    r.step(0);
    r.player->data.hp = 0;
    game::ability_end_pass();
    CHECK(g_calls.back() == "end died");
    CHECK(r.player->data.current_mode == 0);

    g_calls.clear();
    r.player->data.hp = 100;
    r.step(0);
    r.step(16, 113);   // another mod number at once: the first ends `interrupted`, the second starts
    CHECK(r.count("end interrupted") == 1);
    CHECK(r.count("start a:wide") == 1);
    CHECK(game::ability_active_count() == 1);
    game::ability_end_all(EndReason::Gone);
}

TEST_CASE("fourth-ability-slot: world left") {
    // covers: fourth-ability-slot/World left (the unit-test half)
    StepRig r;
    r.step(0);
    r.step(16);
    game::ability_end_all(EndReason::Gone);
    CHECK(r.count("end gone") == 1);
    CHECK(game::ability_active_count() == 0);
    game::ability_end_all(EndReason::Gone);   // nothing is left: no second on_end
    CHECK(r.count("end") == 1);

    // a creature the game destroys: its handle goes, and the pass ends the activation `gone`
    g_calls.clear();
    r.step(0);
    game::on_creature_destroyed(r.player);
    game::ability_end_pass();
    CHECK(g_calls.back() == "end gone");
}

TEST_CASE("fourth-ability-slot: a callback fails") {
    // covers: fourth-ability-slot/A callback fails (the unit-test half: ability-registry/A callback fails)
    StepRig r;
    g_fail_on_step = 1;
    r.step(0);   // on_start, then on_step, which fails; the pass ends it
    CHECK(r.count("start") == 1);
    CHECK(r.count("end cancelled") == 1);   // on_end ran once
    CHECK(r.player->data.current_mode == 0);
    CHECK(game::ability_active_count() == 0);
    game::ability_end_pass();
    CHECK(r.count("end") == 1);

    // ctx:cancel() / ability_end: the mark ends it at the next pass with `cancelled`
    g_fail_on_step = 0;
    g_calls.clear();
    r.player->data.mp = 1.0f;
    r.step(0);
    const KyubuCreature h = game::handle_for(KY_SIDE_CLIENT, r.player);
    CHECK(game::ability_mark_cancel(h));
    game::ability_end_pass();
    CHECK(g_calls.back() == "end cancelled");
    CHECK_FALSE(game::ability_mark_cancel(h));   // no activation left
}

TEST_CASE("fourth-ability-slot: the placeholder runs without callbacks") {
    // covers: fourth-ability-slot/Ends on time (the placeholder: 500 ms, no payload, no callbacks)
    StepRig r;
    r.step(0, 111);
    r.step(480, 111);   // the pass sees 496
    CHECK(game::ability_active_count() == 1);
    r.step(496, 111);   // the pass sees 512: past 500 ms
    CHECK(game::ability_active_count() == 0);
    CHECK(r.player->data.current_mode == 0);
    CHECK(r.player->data.mp == 1.0f);   // cost 0
    CHECK(g_calls.empty());
}

namespace {
std::vector<std::pair<std::string, double>> g_notices;
void rec_notice(const std::string& text, double seconds) { g_notices.push_back({text, seconds}); }
}

TEST_CASE("fourth-ability-slot: the placeholder's text, on the log and the overlay for 3 s") {
    // covers: fourth-ability-slot/The placeholder thing (text, overlay and log; the 500 ms and the sweep are the tests above and the replay)
    CHECK(placeholder_text(1) == "Warrior ability 4: not made yet");
    CHECK(placeholder_text(2) == "Ranger ability 4: not made yet");
    CHECK(placeholder_text(3) == "Mage ability 4: not made yet");
    CHECK(placeholder_text(4) == "Rogue ability 4: not made yet");
    CHECK(placeholder_text(0) == "Class 0 ability 4: not made yet");
    g_notices.clear();
    ability_set_notice(rec_notice);
    {
        StepRig r;
        r.player->data.class_type = 3;
        r.step(0, 111);
        r.step(16, 111);   // a step is not a start
        REQUIRE(g_notices.size() == 1);
        CHECK(g_notices[0].first == "Mage ability 4: not made yet");
        CHECK(g_notices[0].second == 3.0);
        r.step(496, 111);   // ends: the pass sees 512
        CHECK(game::ability_active_count() == 0);
        r.step(0, 111);     // used again
        CHECK(g_notices.size() == 2);
        g_notices.clear();
        r.player->data.current_mode = 0;
        r.step(0, 112);    // a mod ability: no text
        CHECK(g_notices.empty());
    }
    ability_set_notice(nullptr);
    {
        StepRig r;
        r.step(0, 111);   // no overlay: only the log, and no fault
        CHECK(g_notices.empty());
    }
}

TEST_CASE("fourth-ability-slot: [abilities] placeholder = false leaves unclaimed slots empty") {
    // covers: fourth-ability-slot/Turned off (the resolution and the option; the empty slot is the replay)
    const std::string path = std::string(getenv("TEMP") ? getenv("TEMP") : ".") + "\\kyubu_test_placeholder_config.toml";
    auto write = [&](const char* text) { FILE* f = fopen(path.c_str(), "wb"); REQUIRE(f); fputs(text, f); fclose(f); };
    write("[abilities]\nplaceholder = false\n");
    CHECK_FALSE(Config::load(path).ability_placeholder);
    write("[loader]\ndev_mode = true\n");
    CHECK(Config::load(path).ability_placeholder);   // on by default
    write("[abilities]\nplaceholder = true\n");
    CHECK(Config::load(path).ability_placeholder);
    CHECK(Config::load(path).ability_skill_tree);   // on by default (flipped on 2026-10-06)
    write("[abilities]\nskill_tree = false\n");
    CHECK_FALSE(Config::load(path).ability_skill_tree);
    remove(path.c_str());
    Registries& R = reset_registries();
    R.freeze(by_name);
    ability_slots_resolve(false);
    CHECK(ability_slot_holder(3, 0).id.empty());
    CHECK(ability_slot_holder(1, 1).id.empty());
    ability_slots_resolve(true);
    CHECK(ability_slot_holder(3, 0).id == "kyubu:placeholder");
    reset_registries();
}

TEST_CASE("fourth-ability-slot: a Lua ability's callbacks, its state, ctx:cancel and a Lua error") {
    // covers: fourth-ability-slot/Steps counted, ability-registry/A callback fails (the Lua half), ability-registry/The context
    Loaded state;
    Registries& R = reset_registries();
    game::reset_ability_step_for_test();
    Loader& l = L();
    const std::string saved_kyubu = l.kyubu_dir, saved_mods = l.mods_dir;
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    auto temp = [&](const char* name) {
        std::filesystem::path p = std::filesystem::path(tmp) / (std::string("kyubu-ability-") + name + "-" + std::to_string(GetCurrentProcessId()));
        std::error_code ec;
        std::filesystem::remove_all(p, ec);
        std::filesystem::create_directories(p);
        return p.string();
    };
    const std::string game_dir = temp("luarun");
    l.kyubu_dir = game_dir + "/kyubu";
    l.mods_dir = game_dir + "/mods";
    const std::string mod = l.mods_dir + "/fx_run";
    REQUIRE(make_dirs(mod + "/scripts"));
    REQUIRE(write_text(mod + "/mod.toml", "id = \"fx_run\"\nname = \"Abilities: run from Lua\"\n"
                                          "version = \"1.0.0\"\nkyubu = \"^1.0\"\nsides = [\"client\"]\ntrust = \"full\"\n"
                                          "description = \"Unit test: the step runner calls Lua.\"\n\n[scripts]\nclient = \"scripts/client.lua\"\n"));
    REQUIRE(write_text(mod + "/scripts/client.lua", R"(
kyubu.ability.define("fx_run:good", { duration_ms = 800,
  on_start = function(ctx) ctx.state.n = 0; kyubu.info("run good start " .. ctx.id .. " " .. ctx.number .. " " .. type(ctx.creature) .. " rank " .. ctx.rank .. " " .. ctx.side) end,
  on_step = function(ctx)
    ctx.state.n = ctx.state.n + 1
    kyubu.info("run good step " .. ctx.steps .. " " .. ctx.elapsed_ms .. " " .. ctx.dt_ms .. " n=" .. ctx.state.n)
    if ctx.steps == 3 then ctx:cancel() end
  end,
  on_end = function(ctx) kyubu.info("run good end " .. ctx.reason .. " n=" .. ctx.state.n .. " " .. tostring(ctx.steps)) end })
kyubu.ability.define("fx_run:bad", { duration_ms = 800,
  on_step = function(ctx) error("boom") end,
  on_end = function(ctx) kyubu.info("run bad end " .. ctx.reason) end })
kyubu.ability.define("fx_run:quiet", { duration_ms = 800 })
kyubu.ability.define("fx_run:marked", { duration_ms = 800,
  on_start = function(ctx) kyubu.info("run marked start " .. #ctx:marker()) end,
  on_step = function(ctx) kyubu.info("run marked step " .. ctx.steps .. " [" .. table.concat(ctx:marker(), ",") .. "]") end,
  on_end = function(ctx) kyubu.info("run marked end " .. ctx.reason .. " [" .. table.concat(ctx:marker(), ",") .. "]") end })
)"));

    const uint64_t seq = Log::get().sequence();
    auto& mm = ModManager::get();
    mm.discover_and_resolve();
    mm.load_native();
    LuaRuntime::get().load_all();
    Mod* m = mm.find("fx_run");
    REQUIRE(m);
    CHECK(m->state == ModState::Loaded);
    R.freeze(by_name);
    ability_slots_resolve(true);
    uint32_t good = 0, bad = 0, quiet = 0, marked = 0;
    REQUIRE(R.lookup("kyubu:abilities", "fx_run:marked", &marked) == KY_OK);
    REQUIRE(R.lookup("kyubu:abilities", "fx_run:good", &good) == KY_OK);
    REQUIRE(R.lookup("kyubu:abilities", "fx_run:bad", &bad) == KY_OK);
    REQUIRE(R.lookup("kyubu:abilities", "fx_run:quiet", &quiet) == KY_OK);

    cube_World* w = zeroed<cube_World>();
    cube_Creature* player = zeroed<cube_Creature>();
    w->local_player = player;
    player->data.hp = 100;
    auto step = [&](uint32_t mode, uint32_t ms) {
        player->data.current_mode = (uint8_t)mode;
        player->data.mode_start_time = ms;
        game::ability_step_creature(w, player);
        if (player->data.current_mode == mode) player->data.mode_start_time = ms + 16;   // the game's time step, after the site
        game::ability_end_pass();
    };
    // on_start and on_step run, ctx.state lives across the steps, and ctx:cancel() on step 3 ends it `cancelled` at the pass
    step(good, 0);
    step(good, 16);
    CHECK(game::ability_active_count() == 1);
    step(good, 32);
    CHECK(game::ability_active_count() == 0);
    CHECK(player->data.current_mode == 0);
    CHECK(logged(seq, "run good start fx_run:good " + std::to_string(good) + " userdata rank 1 client"));
    CHECK(logged(seq, "run good step 1 0 0 n=1"));
    CHECK(logged(seq, "run good step 2 16 16 n=2"));
    CHECK(logged(seq, "run good step 3 32 16 n=3"));
    CHECK(logged(seq, "run good end cancelled n=3 3"));
    CHECK_FALSE(logged(seq, "run good step 4"));

    // a Lua error in on_step: logged with the mod and the ability, the ability ends `cancelled`, on_end runs once, no fault
    const uint64_t seq2 = Log::get().sequence();
    step(bad, 0);
    CHECK(game::ability_active_count() == 0);
    CHECK(logged(seq2, "Lua error in ability fx_run:bad on_step"));
    CHECK(logged(seq2, "boom"));
    CHECK(logged(seq2, "run bad end cancelled"));
    CHECK(m->state == ModState::Loaded);   // a Lua error is not a fault: the mod's other abilities go on
    game::ability_end_all(game::EndReason::Gone);

    // an ability with no function ends on time, and its state table does not outlive it
    step(quiet, 0);
    step(quiet, 800);
    CHECK(game::ability_active_count() == 0);

    // ctx:marker() is an array of the names crossed since the previous step: empty in on_start, in order, and told once; on_end gets
    // the marker at the clip's end
    bind_marked_clip("fx_run:marked");
    const uint64_t seq3 = Log::get().sequence();
    step(marked, 0);
    step(marked, 500);
    step(marked, 784);
    CHECK(game::ability_active_count() == 0);
    CHECK(logged(seq3, "run marked start 0"));
    CHECK(logged(seq3, "run marked step 1 [start]"));
    CHECK(logged(seq3, "run marked step 2 [wind,hit]"));
    CHECK(logged(seq3, "run marked step 3 []"));
    CHECK(logged(seq3, "run marked end done [done]"));
    Animations::get().clear_bindings();

    VirtualFree(w, 0, MEM_RELEASE);
    VirtualFree(player, 0, MEM_RELEASE);
    game::reset_ability_step_for_test();
    reset_registries();
    l.mods_dir = temp("luarun-empty");
    mm.discover_and_resolve();
    l.kyubu_dir = saved_kyubu;
    l.mods_dir = saved_mods;
}

TEST_CASE("fourth-ability-slot: a native fault in a callback ends the ability cancelled") {
    // covers: fourth-ability-slot/A callback fails (the native half: the guard catches the fault)
    StepRig r;
    g_fault_on_step = 2;
    const uint64_t since = Log::get().sequence();
    r.step(0);
    r.step(16);   // on_step faults here: the guard unwinds, and the pass ends the activation
    CHECK(r.count("start") == 1);
    CHECK(r.count("end cancelled") == 1);
    CHECK(r.player->data.current_mode == 0);
    CHECK(game::ability_active_count() == 0);
    CHECK(logged(since, "loader callback faulted"));
    r.step(32);   // the process and the loop go on
    CHECK(r.count("start") == 2);   // a new press: a new activation, and the old one ended once
    CHECK(r.count("end") == 1);
    game::ability_end_all(EndReason::Gone);
}

TEST_CASE("fourth-ability-slot: a callback over 2 ms logs one warning") {
    // covers: fourth-ability-slot/the 2 ms warning (design Risks)
    StepRig r;
    g_sleep_ms = 5;
    const uint64_t since = Log::get().sequence();
    r.step(0);
    r.step(16);
    r.step(32);
    int warnings = 0;
    for (auto& e : Log::get().since(since))
        if (e.text.find("a:guard on_step took") != std::string::npos && e.text.find("more than 2 ms") != std::string::npos) ++warnings;
    CHECK(warnings == 1);   // three slow steps, one line: at most one per ability every 5 s
    game::ability_end_all(EndReason::Gone);
}

// ---- Learning the fourth ability in the skill tree (task 8.2, design D13) ----

namespace {

// A skill panel as the loader reads it: 11 buttons, each a block with its node at +0x3C, the node a block with its flag index at
// +0x68 and its flag array at +0x94; and the skill-icon map with the null node the panel makes for key 9.
struct FakeSkillPanel {
    uint8_t button[11][0x40] = {};
    uint8_t node[11][0xA0] = {};
    int32_t flags[11][4] = {};
    uintptr_t buttons[11] = {};
    std_tree_node head = {};
    IconNode n9{};
    FakeIconMap abilities;
    cube_GameController* gc = zeroed<cube_GameController>();
    cube_Creature* player = zeroed<cube_Creature>();
    uint8_t widget[0x200] = {};   // the SkillWidget: its 11 working ranks at +0x160, its working specialization at +0x18C
    int32_t* working() { return (int32_t*)(widget + 0x160); }
    int32_t& shown_spec() { return *(int32_t*)(widget + 0x18C); }
    explicit FakeSkillPanel(bool with_node9 = true) {
        for (int i = 0; i < 11; ++i) {
            *(uintptr_t*)(button[i] + 0x3C) = (uintptr_t)node[i];
            *(uint32_t*)(node[i] + 0x68) = 2;   // an index into the flags, not zero
            *(uintptr_t*)(node[i] + 0x94) = (uintptr_t)flags[i];
            buttons[i] = (uintptr_t)button[i];
        }
        gc->skill_buttons.first = buttons;
        gc->skill_buttons.last = buttons + 11;
        gc->skill_buttons.end = buttons + 11;
        head.isnil = 1;
        head.left = head.right = head.parent = &head;
        n9.t = {};
        n9.t.left = n9.t.right = &head;
        n9.key = 9;
        n9.value = 0;
        if (with_node9) {
            n9.t.parent = &head;
            head.parent = &n9.t;
        }
        gc->skill_icons.head = &head;
        gc->skill_icons.size = with_node9 ? 1 : 0;
        gc->ability_icons = abilities.m;
        gc->player = player;
        gc->skill_widget = widget;
        gc->skills_open = 1;
        player->data.class_type = 1;
        player->data.specialization = 1;
        shown_spec() = 1;   // toggleSkills copies the player's specialization into the widget when the panel opens
    }
    int32_t flag(int i) const { return flags[i][2]; }
};

// a:iconic (112) claims the warrior's specialization 1 and shows smash's icon (54); a:plain (113) claims the ranger.
struct TreeRig {
    Loaded state;
    TreeRig() {
        Registries& R = reset_registries();
        DemoMod a("a");
        static const uint32_t cooldown[6] = {6000, 5500, 5000, 4500, 4000, 3500};
        KyubuAbilityDef spec = claim_def(1, 1), any = claim_def(2, -1);
        spec.icon = "cubeworld:smash";
        spec.max_rank = 5;
        spec.cooldown_ms = cooldown;
        REQUIRE(api_table()->ability_define(a.on(KY_SIDE_CLIENT), "a:iconic", &spec) == KY_OK);
        REQUIRE(api_table()->ability_define(a.on(KY_SIDE_CLIENT), "a:plain", &any) == KY_OK);
        R.freeze(by_name);
        ability_slots_resolve(true);
    }
    ~TreeRig() {
        game::set_skilltree_player_for_test(nullptr);
        reset_registries();
    }
};

}  // namespace

TEST_CASE("fourth-ability-slot: the skill tree teaches a mod's ability, not the placeholder") {
    // covers: fourth-ability-slot/Learning the fourth ability (the unit-test half: which pairs get a button)
    TreeRig rig;
    uint32_t n = 0;
    CHECK(game::skilltree_learnable(1, 1, &n));
    CHECK(n == 112);
    CHECK(game::skilltree_learnable(2, 0, &n));   // the class's `any` claim
    CHECK(n == 113);
    CHECK_FALSE(game::skilltree_learnable(1, 0, &n));   // nobody claims it: the placeholder holds it, and is not learned
    CHECK_FALSE(game::skilltree_learnable(0, 0, &n));
    CHECK_FALSE(game::skilltree_learnable(5, 0, &n));
    ability_slots_resolve(false);
    CHECK_FALSE(game::skilltree_learnable(3, 0, &n));   // placeholder off: nobody holds it
}

TEST_CASE("fourth-ability-slot: the skill tree shows button 9 and its icon") {
    // covers: fourth-ability-slot/Learning the fourth ability (the unit-test half: the flag and the icon)
    TreeRig rig;
    SUBCASE("the owner's button is shown, with the named vanilla icon") {
        FakeSkillPanel p;
        CHECK(game::skilltree_step(p.gc) == game::TreeStep::Shown);
        CHECK(p.flag(9) == 1);
        CHECK(p.flag(8) == 0);   // no other button is touched
        CHECK(p.flag(10) == 0);
        CHECK(p.n9.value == 0x1000);   // smash's node value in the ability icon map
        CHECK(p.gc->skill_icons.size == 1);   // no node was inserted
        CHECK(game::skilltree_step(p.gc) == game::TreeStep::Shown);   // the next frame leaves the icon alone
        CHECK(p.n9.value == 0x1000);
    }
    SUBCASE("the panel has not made the icon node yet: the button is shown, nothing is inserted") {
        FakeSkillPanel p(false);
        CHECK(game::skilltree_step(p.gc) == game::TreeStep::Shown);
        CHECK(p.flag(9) == 1);
        CHECK(p.gc->skill_icons.size == 0);
    }
    SUBCASE("the working rank of button 9 stops at the ability's max_rank, the other buttons are left alone") {
        FakeSkillPanel p;
        p.working()[9] = 9;
        p.working()[8] = 9;
        CHECK(game::skilltree_step(p.gc) == game::TreeStep::Shown);
        CHECK(p.working()[9] == 5);   // a:iconic has max_rank 5
        CHECK(p.working()[8] == 9);
        p.working()[9] = 3;
        CHECK(game::skilltree_step(p.gc) == game::TreeStep::Shown);
        CHECK(p.working()[9] == 3);   // under the cap: untouched
        p.gc->skill_widget = nullptr;
        CHECK(game::skilltree_step(p.gc) == game::TreeStep::Shown);   // no widget yet: nothing is read
    }
    SUBCASE("a closed panel, no player, no owner") {
        FakeSkillPanel p;
        p.gc->skills_open = 0;
        CHECK(game::skilltree_step(p.gc) == game::TreeStep::PanelClosed);
        CHECK(p.flag(9) == 0);
        p.gc->skills_open = 1;
        p.gc->player = nullptr;
        CHECK(game::skilltree_step(p.gc) == game::TreeStep::NoPlayer);
        p.gc->player = p.player;
        p.player->data.specialization = 0;   // the placeholder's pair
        p.shown_spec() = 0;
        CHECK(game::skilltree_step(p.gc) == game::TreeStep::NotLearnable);
        CHECK(p.flag(9) == 0);
        CHECK(p.n9.value == 0);
    }
    SUBCASE("a short button vector or a missing node") {
        FakeSkillPanel p;
        p.gc->skill_buttons.last = p.buttons + 9;   // 9 buttons: index 9 is out of range
        CHECK(game::skilltree_step(p.gc) == game::TreeStep::NoButton);
        p.gc->skill_buttons.last = p.buttons + 11;
        *(uintptr_t*)(p.button[9] + 0x3C) = 0;
        CHECK(game::skill_button_flag(p.gc, 9) == nullptr);
        CHECK(game::skill_button_flag(p.gc, 3) == &p.flags[3][2]);
    }
}

TEST_CASE("fourth-ability-slot: button 9 follows the specialization the panel shows") {
    // covers: fourth-ability-slot/Learning the fourth ability (a change of specialization: the owner and the icon follow)
    Loaded state;
    Registries& R = reset_registries();
    DemoMod a("a");
    KyubuAbilityDef zero = claim_def(1, 0), one = claim_def(1, 1);
    zero.icon = "cubeworld:smash";      // smash's node value in the fake ability icon map is 0x1000
    one.icon = "cubeworld:smash";
    REQUIRE(api_table()->ability_define(a.on(KY_SIDE_CLIENT), "a:zero", &zero) == KY_OK);
    REQUIRE(api_table()->ability_define(a.on(KY_SIDE_CLIENT), "a:one", &one) == KY_OK);
    R.freeze(by_name);
    ability_slots_resolve(true);
    game::set_skilltree_player_for_test(nullptr);   // forgets the icon an earlier test wrote

    FakeSkillPanel p;
    uint32_t n = 0;
    CHECK(game::skilltree_shown_specialization(p.gc) == 1);
    CHECK(game::skilltree_step(p.gc) == game::TreeStep::Shown);
    CHECK(p.n9.value == 0x1000);

    p.shown_spec() = 0;   // a click on the other specialization's button, before Learn
    CHECK(game::skilltree_shown_specialization(p.gc) == 0);
    CHECK(game::skilltree_learnable(1, game::skilltree_shown_specialization(p.gc), &n));
    CHECK(n == 113);   // a:zero (numbered by name: a:one is 112)
    p.abilities.n54.value = 0x2000;   // the two abilities' icons differ (a fresh value for smash's node)
    CHECK(game::skilltree_step(p.gc) == game::TreeStep::Shown);
    CHECK(p.n9.value == 0x2000);   // Kyubu's own earlier value is replaced

    p.n9.value = 0x7777;   // a value Kyubu did not write: left alone
    p.abilities.n54.value = 0x1000;
    CHECK(game::skilltree_step(p.gc) == game::TreeStep::Shown);
    CHECK(p.n9.value == 0x7777);

    p.gc->skills_open = 0;   // a closed panel reads the player's own specialization
    p.shown_spec() = 0;
    CHECK(game::skilltree_shown_specialization(p.gc) == 1);
    p.gc->skills_open = 1;
    p.shown_spec() = 9;      // not a specialization: the player's own
    CHECK(game::skilltree_shown_specialization(p.gc) == 1);
    p.gc->skill_widget = nullptr;
    CHECK(game::skilltree_shown_specialization(p.gc) == 1);
    reset_registries();
}

TEST_CASE("fourth-ability-slot: icon_swap replaces only Kyubu's own value") {
    FakeIconMap map;
    const std_map* m = &map.m;
    // node 115 holds null in the fake map (see "the icon map")
    CHECK(game::icon_swap(m, 115, 0xA, 0) == game::IconFill::Written);
    CHECK(game::icon_swap(m, 115, 0xA, 0xA) == game::IconFill::AlreadySet);   // the same value: nothing to do
    CHECK(game::icon_swap(m, 115, 0xB, 0xA) == game::IconFill::Written);
    CHECK(game::icon_swap(m, 115, 0xC, 0xA) == game::IconFill::AlreadySet);   // 0xB is not the value named as Kyubu's
    CHECK(game::icon_swap(m, 115, 0, 0xB) == game::IconFill::NoSource);
    CHECK(game::icon_swap(m, 999, 0xA, 0) == game::IconFill::NoNode);
}

TEST_CASE("fourth-ability-slot: the holder's rank is skills[9], the others' come from the file") {
    // covers: fourth-ability-slot/Learning the fourth ability (the unit-test half: the rank source)
    TreeRig rig;
    FakeSkillPanel p;
    uint32_t rank = 77;
    CHECK_FALSE(game::skilltree_active());
    CHECK_FALSE(game::skilltree_rank_get(112, &rank));   // not installed: the file's rank
    CHECK(rank == 77);

    game::set_skilltree_player_for_test(p.player);
    CHECK(game::skilltree_active());
    CHECK(game::skilltree_rank_get(112, &rank));
    CHECK(rank == 0);   // not learned yet
    CHECK(game::ability_rank_of(112) == 0);

    p.player->data.skills[9] = 3;
    CHECK(game::skilltree_rank_get(112, &rank));
    CHECK(rank == 3);
    CHECK(game::ability_rank_of(112) == 3);
    p.player->data.skills[9] = 9;
    CHECK(game::skilltree_rank_get(112, &rank));
    CHECK(rank == 5);   // clamped to the ability's max_rank

    rank = 77;
    CHECK_FALSE(game::skilltree_rank_get(113, &rank));   // another mod's ability: not this player's holder
    CHECK_FALSE(game::skilltree_rank_get(111, &rank));   // the placeholder
    CHECK_FALSE(game::skilltree_rank_get(54, &rank));    // a vanilla ability
    CHECK(rank == 77);

    uint32_t stored = 0;
    CHECK(game::skilltree_rank_set(112, 2, &stored));
    CHECK(stored == 2);
    CHECK(p.player->data.skills[9] == 2);
    CHECK(game::skilltree_rank_set(112, 40, &stored));
    CHECK(stored == 5);   // clamped
    CHECK(p.player->data.skills[9] == 5);
    CHECK_FALSE(game::skilltree_rank_set(113, 1, &stored));
    CHECK(p.player->data.skills[9] == 5);

    p.player->data.class_type = 2;   // the ranger: a:plain holds its slot now, a:iconic holds nothing for this player
    p.player->data.specialization = 0;
    CHECK_FALSE(game::skilltree_rank_get(112, &rank));
    CHECK(game::skilltree_rank_get(113, &rank));
    CHECK(rank == 2);   // a:plain's max_rank is 2: skills[9] 5 is clamped
}

// ---- A mod's own ability icon (task 8.3, design D14) ----

namespace {

int g_exists_calls = 0, g_load_calls = 0;
bool g_file_served = true;
uint32_t g_loaded_object = 0x5000;
std::string g_loaded_key;
bool fake_exists(const std::string&) { ++g_exists_calls; return g_file_served; }
uint32_t fake_load(cube_GameController*, const std::string& key) { ++g_load_calls; g_loaded_key = key; return g_loaded_object; }
const game::IconFileOps kFakeOps = {fake_exists, fake_load};

// a:iconic (112) claims the warrior's specialization 1 with its own icon file and smash as the fallback; a:vanilla (113) claims the
// ranger with a vanilla icon only.
struct IconFileRig {
    Loaded state;
    IconFileRig() {
        Registries& R = reset_registries();
        DemoMod a("a");
        KyubuAbilityDef own = claim_def(1, 1), plain = claim_def(2, -1);
        own.icon = "cubeworld:smash";
        own.icon_file = "burst.png";
        plain.icon = "cubeworld:smash";
        REQUIRE(api_table()->ability_define(a.on(KY_SIDE_CLIENT), "a:iconic", &own) == KY_OK);
        REQUIRE(api_table()->ability_define(a.on(KY_SIDE_CLIENT), "a:vanilla", &plain) == KY_OK);
        R.freeze(by_name);
        ability_slots_resolve(true);
        g_exists_calls = g_load_calls = 0;
        g_file_served = true;
        g_loaded_object = 0x5000;
        g_loaded_key.clear();
        game::set_icon_file_ops_for_test(&kFakeOps);
    }
    ~IconFileRig() {
        game::set_icon_file_ops_for_test(nullptr);
        reset_registries();
    }
};

}  // namespace

TEST_CASE("fourth-ability-slot: icon_file is checked when the ability is defined") {
    // covers: fourth-ability-slot/A mod's own icon (the unit-test half: the definition)
    Loaded state;
    Registries& R = reset_registries();
    DemoMod a("a");
    auto def_with = [&](const char* file) {
        KyubuAbilityDef d = claim_def(1, 1);
        d.icon_file = file;
        return api_table()->ability_define(a.on(KY_SIDE_CLIENT), "a:iconic", &d);
    };
    CHECK(def_with("burst.png") == KY_OK);
    CHECK(def_with("Burst_2-b.PNG") == KY_E_DUPLICATE);   // a valid name, but another definition of the same id
    CHECK(def_with("a/burst.png") == KY_E_INVALID_ARG);   // no folders
    CHECK(def_with("burst.jpg") == KY_E_INVALID_ARG);
    CHECK(def_with(".png") == KY_E_INVALID_ARG);
    CHECK(def_with("x.png.exe") == KY_E_INVALID_ARG);
    CHECK(def_with("a b.png") == KY_E_INVALID_ARG);
    CHECK(def_with(std::string(61, 'a').append(".png").c_str()) == KY_E_INVALID_ARG);   // 65 characters
    CHECK(def_with(std::string(60, 'a').append(".png").c_str()) == KY_E_DUPLICATE);      // 64 characters
    KyubuAbilityDef none = claim_def(2, -1);
    none.icon_file = "";
    CHECK(api_table()->ability_define(a.on(KY_SIDE_CLIENT), "a:blank", &none) == KY_OK);   // empty is none
    R.freeze(by_name);
    ability_slots_resolve(true);
    REQUIRE(ability_stat(112));   // numbered in id order: a:blank, then a:iconic
    CHECK(ability_stat(112)->icon_file.empty());
    REQUIRE(ability_stat(113));
    CHECK(ability_stat(113)->icon_file == "burst.png");
    reset_registries();
}

TEST_CASE("fourth-ability-slot: a definition made before icon_file existed still loads") {
    // covers: fourth-ability-slot/A mod's own icon (the unit-test half: an older caller)
    Loaded state;
    Registries& R = reset_registries();
    DemoMod a("a");
    KyubuAbilityDef old = claim_def(1, 1);
    old.size = (uint32_t)offsetof(KyubuAbilityDef, icon_file);   // what a mod built before the field passes
    old.icon_file = "ignored.png";                               // not read: outside the caller's struct
    CHECK(api_table()->ability_define(a.on(KY_SIDE_CLIENT), "a:old", &old) == KY_OK);
    KyubuAbilityDef tiny = claim_def(1, 0);
    tiny.size = (uint32_t)offsetof(KyubuAbilityDef, icon_file) - 1;
    CHECK(api_table()->ability_define(a.on(KY_SIDE_CLIENT), "a:tiny", &tiny) == KY_E_INVALID_ARG);
    R.freeze(by_name);
    ability_slots_resolve(true);
    REQUIRE(ability_stat(112));
    CHECK(ability_stat(112)->icon_file.empty());
    reset_registries();
}

TEST_CASE("fourth-ability-slot: the icon file is loaded once and kept") {
    // covers: fourth-ability-slot/A mod's own icon (the unit-test half: one load, one object)
    IconFileRig rig;
    cube_GameController* gc = zeroed<cube_GameController>();
    CHECK(game::ability_file_icon(gc, 112) == 0x5000);
    CHECK(g_loaded_key == "burst.png");
    CHECK(game::ability_file_icon(gc, 112) == 0x5000);
    CHECK(game::ability_file_icon(gc, 112) == 0x5000);
    CHECK(g_load_calls == 1);   // the game never frees the texture: one object per ability
    CHECK(g_exists_calls == 1);
    CHECK(game::ability_file_icon(gc, 113) == 0);   // no icon_file
    CHECK(game::ability_file_icon(gc, 54) == 0);    // a vanilla number
    CHECK(game::ability_file_icon(gc, 111) == 0);   // the placeholder
    CHECK(game::ability_file_icon(nullptr, 112) == 0x5000);   // cached; the controller is only needed to load
    CHECK(g_load_calls == 1);
}

TEST_CASE("fourth-ability-slot: an icon file nothing serves, or that fails to load, falls back") {
    // covers: fourth-ability-slot/A mod's own icon (the unit-test half: the fallback)
    IconFileRig rig;
    cube_GameController* gc = zeroed<cube_GameController>();
    SUBCASE("the asset layer does not serve the key: loadIcon is never called (it would read the disk)") {
        g_file_served = false;
        CHECK(game::ability_file_icon(gc, 112) == 0);
        CHECK(game::ability_file_icon(gc, 112) == 0);
        CHECK(g_load_calls == 0);
        CHECK(g_exists_calls == 1);   // the failure is kept too
    }
    SUBCASE("loadIcon returns null") {
        g_loaded_object = 0;
        CHECK(game::ability_file_icon(gc, 112) == 0);
        CHECK(game::ability_file_icon(gc, 112) == 0);
        CHECK(g_load_calls == 1);
    }
}

TEST_CASE("fourth-ability-slot: the icon goes to the bar's map and the skill tree's, its own first") {
    // covers: fourth-ability-slot/A mod's own icon (the unit-test half: where the pointer goes)
    IconFileRig rig;
    SUBCASE("own icon: the bar's node and button 9's node get the object, nothing is inserted") {
        FakeSkillPanel p;
        FakeIconMap bar_icons;   // keys 0, 54 and a null node 115 (the bar's number is 112 in a real run: the fake only has 115)
        p.gc->ability_icons = bar_icons.m;
        CHECK(game::ability_icon_fill(p.gc, &p.gc->ability_icons, 115, 112) == game::IconFill::Written);
        CHECK(bar_icons.n115.value == 0x5000);
        CHECK(bar_icons.n54.value == 0x1000);   // smash's own is untouched
        CHECK(game::ability_icon_fill(p.gc, &p.gc->skill_icons, 9, 112) == game::IconFill::Written);
        CHECK(p.n9.value == 0x5000);
        CHECK(p.gc->skill_icons.size == 1);
        CHECK(game::ability_icon_fill(p.gc, &p.gc->skill_icons, 9, 112) == game::IconFill::AlreadySet);
        CHECK(g_load_calls == 1);
    }
    SUBCASE("a failing file falls back to the vanilla icon of `icon`") {
        g_file_served = false;
        FakeSkillPanel p;
        CHECK(game::ability_icon_fill(p.gc, &p.gc->skill_icons, 9, 112) == game::IconFill::Written);
        CHECK(p.n9.value == 0x1000);   // smash's node in the ability icon map
    }
    SUBCASE("a mod ability with a vanilla icon only") {
        FakeSkillPanel p;
        CHECK(game::ability_icon_fill(p.gc, &p.gc->skill_icons, 9, 113) == game::IconFill::Written);
        CHECK(p.n9.value == 0x1000);
        CHECK(g_load_calls == 0);
    }
    SUBCASE("no node yet: nothing is written, and the file is still loaded once") {
        FakeSkillPanel p(false);
        CHECK(game::ability_icon_fill(p.gc, &p.gc->skill_icons, 9, 112) == game::IconFill::NoNode);
        CHECK(p.gc->skill_icons.size == 0);
        CHECK(game::ability_icon_fill(p.gc, &p.gc->skill_icons, 9, 112) == game::IconFill::NoNode);
        CHECK(g_load_calls == 1);
    }
    SUBCASE("a number that is not a mod ability") {
        FakeSkillPanel p;
        CHECK(game::ability_icon_fill(p.gc, &p.gc->skill_icons, 9, 54) == game::IconFill::NoSource);
        CHECK(p.n9.value == 0);
    }
    SUBCASE("icon_put refuses a null object and never replaces a value") {
        FakeSkillPanel p;
        CHECK(game::icon_put(&p.gc->skill_icons, 9, 0) == game::IconFill::NoSource);
        CHECK(game::icon_put(&p.gc->skill_icons, 9, 0x77) == game::IconFill::Written);
        CHECK(game::icon_put(&p.gc->skill_icons, 9, 0x88) == game::IconFill::AlreadySet);
        CHECK(p.n9.value == 0x77);
    }
}

TEST_CASE("fourth-ability-slot: the packed definition keeps icon_file") {
    // covers: fourth-ability-slot/A mod's own icon (the unit-test half: both sides get the same payload)
    AbilityData d;
    d.duration_ms = 800;
    d.max_rank = 2;
    d.cooldown_ms = {1, 2, 3};
    d.icon = "cubeworld:smash";
    d.icon_file = "burst.png";
    const std::vector<uint8_t> packed = ability_pack(d);
    AbilityData back;
    REQUIRE(ability_unpack(packed.data(), packed.size(), &back));
    CHECK(back.icon_file == "burst.png");
    CHECK(back.icon == "cubeworld:smash");
    CHECK_FALSE(ability_unpack(packed.data(), packed.size() - 1, &back));
}
