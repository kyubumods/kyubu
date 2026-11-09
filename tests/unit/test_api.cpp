#include "doctest.h"

#include <set>
#include <string>

#include "api.h"
#include "loader.h"
#include "modmanager.h"

using namespace kyubu;

namespace {
void KYUBU_CALL noop_command(const char*, KyubuPrintFn, void*, void*) {}
}  // namespace

TEST_CASE("sides-and-messaging: side rules cover the whole API table") {
    size_t n = 0;
    const ApiSideRule* rules = api_side_rules(&n);
    // every member of KyubuApi after size/version is a function pointer and has exactly one rule
    const size_t members = (sizeof(KyubuApi) - offsetof(KyubuApi, log)) / sizeof(void*);
    CHECK(n == members);
    std::set<std::string> names;
    for (size_t i = 0; i < n; ++i) {
        CHECK(names.insert(rules[i].name).second);
        CHECK((rules[i].sides == KY_SIDE_CLIENT || rules[i].sides == KY_SIDE_SERVER || rules[i].sides == KY_SIDE_BOTH));
    }
    CHECK(api_side_of("ui_begin") == KY_SIDE_CLIENT);
    CHECK(api_side_of("registry_add") == KY_SIDE_BOTH);
    CHECK(api_side_of("no_such_function") == KY_SIDE_NONE);
    CHECK(api_side_allowed("ui_begin", KY_SIDE_NONE));   // outside sided callbacks nothing is refused
}

TEST_CASE("sides-and-messaging: client-only functions refuse server-side callers") {
    // covers: sides-and-messaging/Client code tries an authoritative change (mirror case: server code calls client-only API)
    Loader& l = L();
    l.host.host = KY_HOST_CLIENT;
    l.mode = KY_MODE_MENU_OR_SINGLEPLAYER;
    Mod demo;
    demo.id = "demo";
    demo.state = ModState::Loaded;
    demo.manifest.sides = KY_SIDE_BOTH;
    for (int s = 1; s <= 2; ++s) { demo.ctx[s].mod = &demo; demo.ctx[s].side = (KyubuSide)s; }
    const KyubuApi* api = api_table();

    size_t n = 0;
    const ApiSideRule* rules = api_side_rules(&n);
    int client_only = 0;
    for (size_t i = 0; i < n; ++i) {
        if (rules[i].sides != KY_SIDE_CLIENT) continue;
        ++client_only;
        CHECK_FALSE(api_side_allowed(rules[i].name, KY_SIDE_SERVER));
        CHECK(api_side_allowed(rules[i].name, KY_SIDE_CLIENT));
    }
    CHECK(client_only >= 12);

    // Call the real table entries from a server-side callback in single-player.
    bool ran = ModManager::get().invoke(&demo, KY_SIDE_SERVER, "test", [&] {
        CHECK(api->register_command(demo.handle(KY_SIDE_SERVER), "demo:x", "", noop_command, nullptr) == KY_E_SIDE);
        CHECK(api->imgui_context() == nullptr);
        int open = 1, v = 0;
        float f = 0;
        char buf[8] = {};
        CHECK(api->ui_begin("demo", &open) == 0);
        CHECK(api->ui_button("b") == 0);
        CHECK(api->ui_checkbox("c", &v) == 0);
        CHECK(api->ui_slider_float("s", &f, 0, 1) == 0);
        CHECK(api->ui_input_text("t", buf, sizeof buf) == 0);
        CHECK(api->ui_want_input() == 0);
        api->ui_text("t");
        api->ui_same_line();
        api->ui_separator();
        api->ui_end();
        // functions for both sides still work
        uint32_t num = 0;
        CHECK(api->registry_lookup("kyubu:models", "cubeworld:airship", &num) != KY_E_SIDE);
    });
    CHECK(ran);
}
