// kyubu_probe's call tracer on the hook-test functions of test_hooks.cpp, through the real API table and hook
// engine, and the probe driven the way a replay drives it: a script (add-mapping-workspace tasks 5.1, 5.4).
#include "doctest.h"

#include <windows.h>

#include <string>
#include <vector>

#include "api.h"
#include "channels.h"
#include "console.h"
#include "events.h"
#include "hooks.h"
#include "loader.h"
#include "log.h"
#include "modmanager.h"
#include "probe.h"
#include "util.h"

using namespace kyubu;

extern "C" int __cdecl target_add(int a, int b);   // tests/hook/test_hooks.cpp
extern "C" int __cdecl target_mul(int a, int b);

namespace {

Mod& tracer_mod() {
    static Mod m;
    if (m.id.empty()) {
        m.id = "kyubu_probe";
        m.state = ModState::Loaded;
        m.manifest.sides = KY_SIDE_BOTH;
        for (int s = 1; s <= 2; ++s) { m.ctx[s].mod = &m; m.ctx[s].side = (KyubuSide)s; }
    }
    return m;
}

std::vector<std::string> console(const std::string& line) {
    static bool builtins = (register_builtin_commands(), true);   // hooks, help... as at boot
    (void)builtins;
    std::vector<std::string> out;
    Console::get().execute(line, [&](const std::string& s) { out.push_back(s); });
    return out;
}

int (__cdecl* volatile g_add)(int, int) = target_add;   // called through a pointer, so the calls really happen
int (__cdecl* volatile g_mul)(int, int) = target_mul;

#ifdef _MSC_VER
#define NOINLINE __declspec(noinline)
#else
#define NOINLINE __attribute__((noinline))
#endif

// A __thiscall method whose callee pops three stack arguments, like cube::World::update.
struct Stepper {
    int total = 0;
    NOINLINE void step(int dt, int* out, int flag) {
        volatile int d = dt;
        total += d;
        if (out) *out = total * 2 + flag;
    }
};

uintptr_t method_address(void (Stepper::*m)(int, int*, int)) {
#ifdef _MSC_VER
    static_assert(sizeof m == sizeof(uintptr_t), "a plain class's member pointer is the function's address");
#else
    // Itanium ABI (MinGW): {function address, this adjustment}; the first word is the address of a non-virtual method
    static_assert(sizeof m == 2 * sizeof(uintptr_t), "a plain class's member pointer is the address and an adjustment");
#endif
    uintptr_t a = 0;
    memcpy(&a, &m, sizeof a);
    return a;
}

}  // namespace

TEST_CASE("live-probes: a detour pops the prototype's stack arguments") {
    // add-game-api task 2.1: a pass-through detour with the prototype's argument count runs; the game's code keeps
    // working, so a wrong count (which would unbalance the stack at once) is ruled out.
    const KyubuApi* api = api_table();
    Mod& m = tracer_mod();
    kprobe::Tracer tracer(api);
    std::string err;
    const std::string at = kprobe::strf("0x%X", (unsigned)method_address(&Stepper::step));
    CHECK_FALSE(tracer.add_detour(at, kprobe::kMaxDetourArgs + 1, &err));
    CHECK(err.find("0 to 4 stack arguments") != std::string::npos);
    REQUIRE(tracer.add_detour(at, 3, &err));
    std::vector<std::string> report;
    REQUIRE(tracer.start(m.handle(KY_SIDE_SERVER), &report) == 1);
    CHECK(report[0].find("detouring (3 stack argument(s)) " + at) == 0);
    CHECK(Hooks::get().list("kyubu_probe").size() == 1);

    Stepper s;
    int out = 0;
    s.step(20, &out, 0);
    s.step(7, &out, 1);
    CHECK(s.total == 27);
    CHECK(out == 55);
    CHECK(tracer.count(at) == 2);
    auto calls = tracer.recent(at);
    REQUIRE(calls.size() == 2);
    CHECK(calls[1].nargs == 3);
    CHECK(calls[1].args[0] == 7);
    CHECK(calls[1].args[1] == (uint32_t)(uintptr_t)&out);
    CHECK(calls[1].args[2] == 1);
    CHECK(calls[1].ecx == (uint32_t)(uintptr_t)&s);
    CHECK_FALSE(calls[1].has_regs);                                       // a detour records no registers
    HMODULE self = nullptr;                                               // returns into this test executable
    CHECK(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             (LPCWSTR)(uintptr_t)calls[1].ret, &self));
    CHECK(self == GetModuleHandleW(nullptr));

    CHECK(tracer.stop() == 1);
    CHECK(Hooks::get().list("kyubu_probe").empty());
    s.step(3, &out, 0);
    CHECK(s.total == 30);
    CHECK(tracer.count(at) == 2);

    // the slot is free again: a second run hooks it once more
    REQUIRE(tracer.start(m.handle(KY_SIDE_CLIENT), &report) == 1);
    s.step(1, nullptr, 0);
    CHECK(tracer.count(at) == 1);
    CHECK(tracer.stop() == 1);
}

TEST_CASE("live-probes: the call tracer on the hook-test functions") {
    // covers: live-probes/Clean stop
    const KyubuApi* api = api_table();
    Mod& m = tracer_mod();
    kprobe::Tracer tracer(api);
    std::string err;
    std::string add = kprobe::strf("0x%X", (unsigned)(uintptr_t)&target_add);
    std::string mul = kprobe::strf("0x%X", (unsigned)(uintptr_t)&target_mul);
    REQUIRE(tracer.add(add, &err));
    REQUIRE(tracer.add(mul, &err));
    CHECK_FALSE(tracer.add(add, &err));                                   // traced once only
    // A function that starts with a relative jump is refused by the hook engine: reported, not traced.
    auto code = (uint8_t*)VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    code[0] = 0xE9; *(int32_t*)(code + 1) = 0; code[5] = 0xC3;
    REQUIRE(tracer.add(kprobe::strf("0x%X", (unsigned)(uintptr_t)code), &err));
    tracer.set_args(2);

    std::vector<std::string> report;
    CHECK(tracer.start(m.handle(KY_SIDE_CLIENT), &report) == 2);
    REQUIRE(report.size() == 3);
    CHECK(report[2].find("already starts with a jump") != std::string::npos);
    CHECK(Hooks::get().list("kyubu_probe").size() == 2);

    CHECK(g_add(2, 3) == 5);
    tracer.mark("key 0x51");
    CHECK(g_add(7, 8) == 15);
    CHECK(g_add(1, 1) == 2);
    CHECK(g_mul(3, 4) == 12);
    CHECK(tracer.count(add) == 3);
    CHECK(tracer.count(mul) == 1);
    auto calls = tracer.recent(add);
    REQUIRE(calls.size() == 3);
    CHECK(calls[1].args[0] == 7);                                         // the stack arguments
    CHECK(calls[1].args[1] == 8);
    CHECK(calls[1].nargs == 2);
    CHECK(calls[1].thread == GetCurrentThreadId());
    CHECK(calls[1].has_regs);                                             // a mid hook records the registers
    HMODULE self = nullptr;                                               // returns into this test executable
    CHECK(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             (LPCWSTR)(uintptr_t)calls[1].ret, &self));
    CHECK(self == GetModuleHandleW(nullptr));
    auto lines = tracer.report(2);
    CHECK(lines[0].find("3 call(s)") != std::string::npos);
    CHECK(lines[1].find("  eax=") != std::string::npos);                  // the report shows them
    CHECK(lines[1].find(" esi=") != std::string::npos);
    CHECK(lines.back().find("key 0x51") != std::string::npos);

    // Clean stop: every hook the tracer installed is gone, and the functions behave as before.
    CHECK(tracer.stop() == 2);
    CHECK(Hooks::get().list("kyubu_probe").empty());
    CHECK(console("hooks kyubu_probe").empty());
    CHECK(g_add(2, 3) == 5);
    CHECK(tracer.count(add) == 3);
    VirtualFree(code, 0, MEM_RELEASE);
}

TEST_CASE("live-probes: the probe in dev mode, driven by a script") {
    // The probe as the loader starts it in dev mode, then a probe.cfg-style script that traces a function,
    // checks its expectations and stops, as a replay does in the game.
    L().host.host = KY_HOST_CLIENT;
    L().dev_mode = true;
    Mod& m = tracer_mod();
    const KyubuApi* api = api_table();
    REQUIRE(kprobe::Probe::init(api, m.handle(KY_SIDE_CLIENT), KY_SIDE_CLIENT) == 0);
    REQUIRE(kprobe::Probe::init(api, m.handle(KY_SIDE_SERVER), KY_SIDE_SERVER) == 0);
    kprobe::Probe* p = kprobe::Probe::get();
    REQUIRE(p);
    void* iface = nullptr;
    CHECK(api->get_interface(KYUBU_PROBE_FINDER, 1, &iface, nullptr) == KY_OK);   // the finder for Lua and C
    auto help = console("kyubu_probe:help");
    REQUIRE(!help.empty());
    CHECK(help[0].find("kyubu_probe (dev mode)") == 0);

    std::string mul = kprobe::strf("0x%X", (unsigned)(uintptr_t)&target_mul);
    std::string script =
        "# a comment\n"
        "kyubu_probe:trace add " + mul + "\n"
        "trace start\n"
        "wait 0.5\n"
        "trace stop\n"
        "trace report\n"
        "expect test::mul calls " + mul + " == 2\n"
        "expect test::mul calls " + mul + " > 5\n"
        "expect test::mul arg " + mul + " 1 >= 2\n"
        "expect test::mul arg " + mul + " 2 == 0x2\n"
        "expect test::mul reg " + mul + " esi >= 0\n"
        "expect test::mul reg " + mul + " eax < 0\n"
        "expect - hooks == 0\n";
    std::string err;
    uint64_t seq = Log::get().sequence();
    REQUIRE(p->start_script(script, KY_SIDE_CLIENT, &err));
    for (int i = 0; i < 200 && !p->tracer().running(); ++i) Sleep(5);
    REQUIRE(p->tracer().running());
    CHECK(g_mul(2, 2) == 4);
    CHECK(g_mul(3, 3) == 9);
    for (int i = 0; i < 400 && p->script_running(); ++i) Sleep(5);
    CHECK_FALSE(p->script_running());
    CHECK(p->tally().held == 4);
    CHECK(p->tally().failed == 3);
    std::string log;
    for (auto& l : Log::get().since(seq)) log += l.text + "\n";
    CHECK(log.find("script started: 12 line(s)") != std::string::npos);
    CHECK(log.find("expect test::mul: held (" + mul + ": 2 call(s), expected == 2)") != std::string::npos);
    CHECK(log.find("expect test::mul: FAILED (" + mul + ": 2 call(s), expected > 5)") != std::string::npos);
    CHECK(log.find("expect test::mul: held (2 of the last 2 call(s) of " + mul + " had argument 1 >= 2 (seen 0x2 to 0x3))") !=
          std::string::npos);
    CHECK(log.find("expect test::mul: FAILED (1 of the last 2 call(s) of " + mul + " had argument 2 == 0x2") !=
          std::string::npos);
    CHECK(log.find("expect test::mul: held (2 of the last 2 call(s) of " + mul + " had esi >= 0 (seen 0x") != std::string::npos);
    CHECK(log.find("expect test::mul: FAILED (0 of the last 2 call(s) of " + mul + " had eax < 0 (seen 0x") != std::string::npos);
    CHECK(log.find("script finished: 7 expectation(s), 4 held, 3 failed") != std::string::npos);
    CHECK(Hooks::get().list("kyubu_probe").empty());

    // The console runs the same commands.
    auto out = console("kyubu_probe:trace report");
    REQUIRE(!out.empty());
    CHECK(out[0].find("2 call(s)") != std::string::npos);
    CHECK(console("kyubu_probe:find")[0].find("no search") == 0);

    kprobe::Probe::shutdown(KY_SIDE_CLIENT);
    CHECK(kprobe::Probe::get() == p);                                     // the server side is still active
    kprobe::Probe::shutdown(KY_SIDE_SERVER);
    CHECK(kprobe::Probe::get() == nullptr);
    CHECK(console("kyubu_probe:help").empty());                           // the commands do nothing any more
    Console::get().remove_mod(&m);
    Interfaces::get().remove_mod(&m);
    L().dev_mode = false;
}
