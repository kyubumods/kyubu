#include "doctest.h"

#include "join_gate.h"
#include "manifest.h"

using namespace kyubu;
using namespace kyubu::game;

TEST_CASE("join gate: the value does not depend on the load order") {
    const GateMod a{"level_rework", "1.2.0"}, b{"skills_unleashed", "0.3.0"}, c{"zeta", "1.0.0"};
    const uint32_t v = gate_value({a, b, c});
    CHECK(v == gate_value({c, b, a}));
    CHECK(v == gate_value({b, c, a}));
    CHECK(v == gate_value({a, c, b}));
    CHECK(v != 0);
}

TEST_CASE("join gate: another set or another version gives another value") {
    const GateMod a{"level_rework", "1.2.0"}, b{"skills_unleashed", "0.3.0"};
    const uint32_t both = gate_value({a, b});
    CHECK(both != gate_value({a}));
    CHECK(both != gate_value({b}));
    CHECK(both != gate_value({a, GateMod{"skills_unleashed", "0.3.1"}}));
    CHECK(gate_value({a}) != gate_value({GateMod{"level_rework", "1.2.1"}}));
    CHECK(gate_value({GateMod{"ab", "1"}, GateMod{"c", "1"}}) != gate_value({GateMod{"a", "1"}, GateMod{"bc", "1"}}));
}

TEST_CASE("join gate: no gate mods means no gate, and the value is never a reserved number") {
    CHECK(gate_value({}) == 0);
    for (int i = 0; i < 2000; ++i) {
        const uint32_t v = gate_value({GateMod{"m" + std::to_string(i), "1.0." + std::to_string(i)}});
        CHECK(v != 0);
        CHECK(v != 3);
        CHECK(v != 17);
    }
}

TEST_CASE("join gate: the server rewrites the received version") {
    const uint32_t g = gate_value({GateMod{"a", "1.0.0"}});
    CHECK(gate_accept_slot(g, g) == 3);          // a client with the same gate mods joins
    CHECK(gate_accept_slot(3, g) == 0);          // a vanilla client is refused by the game's own compare
    CHECK(gate_accept_slot(g + 1, g) == g + 1);  // another gate stays and is refused (not 3)
    CHECK(gate_accept_slot(g + 1, g) != 3);
    CHECK(gate_accept_slot(0, g) != 3);
}

TEST_CASE("join gate: the patch bytes") {
    const auto b = gate_bytes(0x4B4C0001);
    CHECK(b[0] == 0x01);
    CHECK(b[1] == 0x00);
    CHECK(b[2] == 0x4C);
    CHECK(b[3] == 0x4B);
}

TEST_CASE("join gate: checked bytes of the client store") {
    const uint8_t ok[10] = {0xC7, 0x85, 0x0C, 0xDA, 0xFF, 0xFF, 0x03, 0x00, 0x00, 0x00};
    size_t off = 0;
    CHECK(gate_client_bytes_ok(ok, sizeof ok, &off));
    CHECK(off == 6);
    uint8_t bad[10];
    memcpy(bad, ok, 10);
    bad[6] = 0x4B;   // already another value: refused, nothing is patched over it
    CHECK_FALSE(gate_client_bytes_ok(bad, sizeof bad, &off));
    memcpy(bad, ok, 10);
    bad[0] = 0xC6;
    CHECK_FALSE(gate_client_bytes_ok(bad, sizeof bad, &off));
    CHECK_FALSE(gate_client_bytes_ok(ok, 9, &off));
}

TEST_CASE("join gate: checked bytes of the server compare") {
    const uint8_t ok[7] = {0x83, 0xBD, 0x44, 0xFE, 0xFF, 0xFF, 0x03};
    int32_t disp = 0;
    CHECK(gate_server_bytes_ok(ok, sizeof ok, &disp));
    CHECK(disp == -0x1BC);
    uint8_t bad[7];
    memcpy(bad, ok, 7);
    bad[6] = 0x04;
    CHECK_FALSE(gate_server_bytes_ok(bad, sizeof bad, &disp));
    memcpy(bad, ok, 7);
    bad[1] = 0x7D;
    CHECK_FALSE(gate_server_bytes_ok(bad, sizeof bad, &disp));
    CHECK_FALSE(gate_server_bytes_ok(ok, 6, &disp));
}

TEST_CASE("join gate: an empty set installs nothing") {
    CHECK(join_gate_set({}) == 0);
    CHECK(join_gate_value() == 0);
}

TEST_CASE("join gate: mod.toml key gate") {
    std::vector<std::string> e;
    const std::string base = "id = \"gm\"\nname = \"x\"\nversion = \"1.0.0\"\nkyubu = \"^1.0\"\nsides = [\"client\"]\n";
    auto m = Manifest::parse(base + "gate = true\n", "mod.toml", e);
    REQUIRE(m);
    CHECK(m->gate);
    auto n = Manifest::parse(base, "mod.toml", e);
    REQUIRE(n);
    CHECK_FALSE(n->gate);
}
