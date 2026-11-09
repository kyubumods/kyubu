#include "doctest.h"

#include "registry.h"

#include <algorithm>
#include <cstdio>

using namespace kyubu;

namespace {
int order_of(const std::string& o) { return o == "demo" ? 0 : o == "b" ? 1 : o == "lib" ? 2 : 100; }

// Registries is a singleton; each test gets a fresh one through placement tricks is overkill, so tests
// use distinct registry names and run in the order below.
Registries& fresh() {
    static Registries* r = nullptr;
    delete r;
    r = new Registries();
    r->init_builtin();
    return *r;
}
}  // namespace

TEST_CASE("content-registries: vanilla lookup") {
    // covers: content-registries/Vanilla lookup
    Registries& R = fresh();
    uint32_t n = 0;
    CHECK(R.lookup("kyubu:models", "cubeworld:airship", &n) == KY_OK);
    CHECK(n == 2563);
    CHECK(R.lookup("kyubu:models", "cubeworld:iron-sword1", &n) == KY_OK);
    CHECK(n == 899);
    // a file loaded into several slots: plain id aliases the lowest slot, "#slot" ids name each one
    std::string name;
    CHECK(R.name_of("kyubu:models", 899, &name) == KY_OK);
    CHECK(name == "cubeworld:iron-sword1");
}

TEST_CASE("content-registries: namespaces, phases, freeze and numbering") {
    // covers: content-registries/Foreign namespace
    // covers: content-registries/Re-launch stability
    Registries& R = fresh();
    CHECK(R.add("a", "kyubu:models", "b:thing", "x", 1, nullptr) == KY_E_NAMESPACE);
    CHECK(R.add("demo", "kyubu:models", "demo:crate", "demo-crate.cub", 14, nullptr) == KY_OK);
    CHECK(R.add("b", "kyubu:models", "b:alpha", "b-alpha.cub", 11, nullptr) == KY_OK);
    CHECK(R.add("demo", "kyubu:models", "demo:crate", "x", 1, nullptr) == KY_E_DUPLICATE);
    CHECK(R.add("demo", "kyubu:models", "demo:crate", "demo-crate.cub", 14, nullptr) == KY_OK);   // same entry again
    uint32_t n = 0;
    CHECK(R.lookup("kyubu:models", "demo:crate", &n) == KY_E_PHASE);   // numbers only exist after freeze
    R.freeze(order_of);
    CHECK(R.add("demo", "kyubu:models", "demo:late", "late.cub", 8, nullptr) == KY_E_PHASE);
    CHECK(R.lookup("kyubu:models", "demo:crate", &n) == KY_OK);
    CHECK(n == 2569);   // demo loads first
    CHECK(R.lookup("kyubu:models", "b:alpha", &n) == KY_OK);
    CHECK(n == 2570);
    std::string h1;
    R.hash("kyubu:models", &h1);

    // same inputs -> same numbers and hash
    Registries& R2 = fresh();
    R2.add("b", "kyubu:models", "b:alpha", "b-alpha.cub", 11, nullptr);
    R2.add("demo", "kyubu:models", "demo:crate", "demo-crate.cub", 14, nullptr);
    R2.freeze(order_of);
    std::string h2;
    R2.hash("kyubu:models", &h2);
    CHECK(h1 == h2);
    CHECK(h1.size() == 64);
}

TEST_CASE("content-registries: remap, latest wins") {
    Registries& R = fresh();
    CHECK(R.remap("demo", "kyubu:models", "cubeworld:iron-sword1", "first.cub", 9) == KY_OK);
    CHECK(R.remap("b", "kyubu:models", "cubeworld:iron-sword1", "gold-sword.cub", 14) == KY_OK);
    R.freeze(order_of);
    std::vector<uint8_t> p;
    CHECK(R.payload("kyubu:models", "cubeworld:iron-sword1", &p) == KY_OK);
    CHECK(std::string(p.begin(), p.end()) == "gold-sword.cub");
}

TEST_CASE("content-registries: custom registries") {
    // covers: content-registries/Registry owned by a library mod
    Registries& R = fresh();
    CHECK(R.create("lib", "lib:spells", 1, true) == KY_OK);
    CHECK(R.create("b", "lib:other", 1, true) == KY_E_NAMESPACE);
    CHECK(R.create("lib", "lib:spells", 1, true) == KY_OK);          // same registry again (the owner's other side)
    CHECK(R.create("lib", "lib:spells", 2, true) == KY_E_DUPLICATE); // a different definition clashes
    CHECK(R.add("b", "lib:spells", "b:frostnova", "{}", 2, nullptr) == KY_OK);
    R.freeze(order_of);
    uint32_t c = 0, num = 0;
    CHECK(R.count("lib:spells", &c) == KY_OK);
    CHECK(c == 1);
    std::string id;
    CHECK(R.entry("lib:spells", 0, &id, &num) == KY_OK);
    CHECK(id == "b:frostnova");
}

TEST_CASE("content-registries: per-save id maps") {
    // covers: content-registries/Mod removed
    Registries& R = fresh();
    R.add("demo", "kyubu:models", "demo:crate", "demo-crate.cub", 14, nullptr);
    R.add("b", "kyubu:models", "b:alpha", "b-alpha.cub", 11, nullptr);
    R.freeze(order_of);
    std::string saved = R.idmap_json();   // demo:crate=2569, b:alpha=2570

    // Next session: "demo" is gone, a new mod adds "lib:new".
    Registries& N = fresh();
    N.add("b", "kyubu:models", "b:alpha", "b-alpha.cub", 11, nullptr);
    N.add("lib", "kyubu:models", "lib:new", "new.cub", 7, nullptr);
    N.freeze(order_of);
    uint32_t n = 0;
    N.lookup("kyubu:models", "b:alpha", &n);
    CHECK(n == 2569);   // fresh numbering before the save is applied
    std::vector<std::pair<std::string, std::string>> moved;
    auto missing = N.apply_idmap(saved, &moved);
    CHECK(missing == std::vector<std::string>{"demo"});
    N.lookup("kyubu:models", "b:alpha", &n);
    CHECK(n == 2570);   // keeps its saved number
    N.lookup("kyubu:models", "demo:crate", &n);
    CHECK(n == 2569);   // reserved placeholder
    N.lookup("kyubu:models", "lib:new", &n);
    CHECK(n != 2569);
    CHECK(n != 2570);
    const Registry* reg = N.get_registry("kyubu:models");
    CHECK(reg->find("demo:crate")->placeholder);
}

TEST_CASE("ability-registry: vanilla names") {
    // covers: ability-registry/Vanilla names
    Registries& R = fresh();
    uint32_t n = 0;
    CHECK(R.lookup("kyubu:abilities", "cubeworld:smash", &n) == KY_OK);   // before the freeze too: fixed numbers
    CHECK(n == 54);
    R.freeze(order_of);
    CHECK(R.lookup("kyubu:abilities", "cubeworld:smash", &n) == KY_OK);
    CHECK(n == 54);
    CHECK(R.lookup("kyubu:abilities", "cubeworld:mode_57", &n) == KY_OK);
    CHECK(n == 57);
    CHECK(R.lookup("kyubu:abilities", "cubeworld:teleport_to_city", &n) == KY_OK);
    CHECK(n == 105);
    CHECK(R.lookup("kyubu:abilities", "kyubu:placeholder", &n) == KY_OK);
    CHECK(n == 111);
    std::string name;
    CHECK(R.name_of("kyubu:abilities", 54, &name) == KY_OK);
    CHECK(name == "cubeworld:smash");
    CHECK(R.name_of("kyubu:abilities", 112, &name) == KY_E_NOT_FOUND);   // nothing defined
    uint32_t count = 0;
    R.count("kyubu:abilities", &count);
    CHECK(count == 112);   // 0..110 and the placeholder
    const Registry* r = R.get_registry("kyubu:abilities");
    CHECK(!r->persistent);
}

TEST_CASE("ability-registry: mod numbers follow the load order, and are released") {
    // covers: ability-registry/Mod numbers follow the load order
    // covers: ability-registry/Released when a mod is removed
    Registries& R = fresh();
    // "b" is defined first on purpose: the load order decides, not the call order.
    CHECK(R.add_defined("b", "kyubu:abilities", "b:second", "y", 1) == KY_OK);
    CHECK(R.add_defined("demo", "kyubu:abilities", "demo:first", "x", 1) == KY_OK);
    uint32_t n = 0;
    CHECK(R.lookup("kyubu:abilities", "demo:first", &n) == KY_E_PHASE);
    R.freeze(order_of);
    CHECK(R.lookup("kyubu:abilities", "demo:first", &n) == KY_OK);
    CHECK(n == 112);
    CHECK(R.lookup("kyubu:abilities", "b:second", &n) == KY_OK);
    CHECK(n == 113);
    std::string h1;
    CHECK(R.hash("kyubu:abilities", &h1) == KY_OK);
    CHECK(h1.size() == 64);
    auto names = R.names();
    CHECK(std::find(names.begin(), names.end(), "kyubu:abilities") != names.end());   // the manifest lists every registry

    std::string saved = R.idmap_json();   // fresh() frees R: take what the rest needs now
    // Same mods, the other call order: same numbers, same hash.
    Registries& R2 = fresh();
    R2.add_defined("demo", "kyubu:abilities", "demo:first", "x", 1);
    R2.add_defined("b", "kyubu:abilities", "b:second", "y", 1);
    R2.freeze(order_of);
    std::string h2;
    R2.hash("kyubu:abilities", &h2);
    CHECK(h1 == h2);

    // Next start: "demo" is removed. No id map keeps 112 for it, and the id map text holds no ability entry.
    CHECK(saved.find("kyubu:abilities") == std::string::npos);
    Registries& N = fresh();
    N.add_defined("b", "kyubu:abilities", "b:second", "y", 1);
    N.add_defined("lib", "kyubu:abilities", "lib:other", "z", 1);
    N.freeze(order_of);
    N.apply_idmap(saved, nullptr);
    CHECK(N.lookup("kyubu:abilities", "b:second", &n) == KY_OK);
    CHECK(n == 112);
    CHECK(N.lookup("kyubu:abilities", "lib:other", &n) == KY_OK);
    CHECK(n == 113);
    CHECK(N.lookup("kyubu:abilities", "demo:first", &n) == KY_E_NOT_FOUND);   // no placeholder reserved for it
}

TEST_CASE("ability-registry: no add, no remap") {
    // covers: ability-registry/No remap
    Registries& R = fresh();
    CHECK(R.add("demo", "kyubu:abilities", "demo:plain", "x", 1, nullptr) == KY_E_INVALID_ARG);
    CHECK(R.remap("demo", "kyubu:abilities", "cubeworld:smash", "x", 1) == KY_E_INVALID_ARG);
    CHECK(R.remap("demo", "kyubu:abilities", "kyubu:placeholder", "x", 1) == KY_E_INVALID_ARG);
    CHECK(R.get_registry("kyubu:abilities")->remaps.empty());
    uint32_t count = 0;
    R.count("kyubu:abilities", &count);
    CHECK(count == 112);   // unchanged
    // The definition path still checks the namespace and repeats.
    CHECK(R.add_defined("demo", "kyubu:abilities", "b:wrong", "x", 1) == KY_E_NAMESPACE);
    CHECK(R.add_defined("demo", "kyubu:abilities", "demo:one", "x", 1) == KY_OK);
    CHECK(R.add_defined("demo", "kyubu:abilities", "demo:one", "x", 1) == KY_OK);   // same data again
    CHECK(R.add_defined("demo", "kyubu:abilities", "demo:one", "y", 1) == KY_E_DUPLICATE);
    // Other registries are unchanged: add stays open there and the definition path is not special.
    CHECK(R.add("demo", "kyubu:models", "demo:crate", "demo-crate.cub", 14, nullptr) == KY_OK);
}

TEST_CASE("ability-registry: over the cap") {
    // covers: ability-registry/Over the cap
    Registries& R = fresh();
    for (int i = 0; i < 144; ++i) {
        char id[32];
        snprintf(id, sizeof id, "demo:a%03d", i);
        CHECK(R.add_defined("demo", "kyubu:abilities", id, "x", 1) == KY_OK);
    }
    CHECK(R.add_defined("b", "kyubu:abilities", "b:last", "x", 1) == KY_OK);   // the 145th, last in order
    R.freeze(order_of);
    uint32_t n = 0;
    CHECK(R.lookup("kyubu:abilities", "demo:a000", &n) == KY_OK);
    CHECK(n == 112);
    CHECK(R.lookup("kyubu:abilities", "demo:a143", &n) == KY_OK);
    CHECK(n == 255);
    CHECK(R.lookup("kyubu:abilities", "b:last", &n) == KY_E_NOT_FOUND);
    std::string name;
    CHECK(R.name_of("kyubu:abilities", 255, &name) == KY_OK);
    CHECK(name == "demo:a143");
    CHECK(R.lookup("kyubu:abilities", "kyubu:placeholder", &n) == KY_OK);
    CHECK(n == 111);
    uint32_t count = 0;
    R.count("kyubu:abilities", &count);
    CHECK(count == 112 + 144);
}
