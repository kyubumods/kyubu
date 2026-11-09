// Hook-engine tests: run the real hook framework against dummy functions in this executable.
#include "doctest.h"

#include <cstdlib>

#include "guard.h"
#include "hooks.h"
#include "symbols.h"

using namespace kyubu;

#ifdef _MSC_VER
#define NOINLINE __declspec(noinline)
#else
#define NOINLINE __attribute__((noinline))
#endif

extern "C" NOINLINE int __cdecl target_add(int a, int b) {
    volatile int x = a;
    return x + b;
}
extern "C" NOINLINE int __cdecl target_mul(int a, int b) {
    volatile int x = a;
    return x * b;
}

// ---- chained function hooks
static int (__cdecl* next_a)(int, int) = nullptr;
static int (__cdecl* next_b)(int, int) = nullptr;
static std::string trace;

extern "C" int __cdecl hook_a(int a, int b) { trace += "a"; return next_a(a, b) + 1000; }
extern "C" int __cdecl hook_b(int a, int b) { trace += "b"; return next_b(a, b) + 100; }

TEST_CASE("hook-framework: two mods hook the same function, then unhook in the middle") {
    // covers: hook-framework/Two mods hook the same function
    // covers: hook-framework/Unhook in the middle
    auto& h = Hooks::get();
    h.owner_order = [](const std::string& o) { return o == "a" ? 0 : 1; };
    KyubuId ia = 0, ib = 0;
    REQUIRE(h.hook_function("a", (uintptr_t)&target_add, (void*)&hook_a, 0, (void**)&next_a, &ia) == KY_OK);
    REQUIRE(h.hook_function("b", (uintptr_t)&target_add, (void*)&hook_b, 0, (void**)&next_b, &ib) == KY_OK);
    trace.clear();
    CHECK(target_add(2, 3) == 5 + 100 + 1000);   // a -> b -> original, a's result returned
    CHECK(trace == "ab");
    REQUIRE(h.remove("a", ia) == KY_OK);
    trace.clear();
    CHECK(target_add(2, 3) == 105);
    CHECK(trace == "b");
    REQUIRE(h.remove("b", ib) == KY_OK);
    CHECK(target_add(2, 3) == 5);
    CHECK(h.count("a") == 0);
}

TEST_CASE("hook-framework: priority overrides load order") {
    auto& h = Hooks::get();
    h.owner_order = [](const std::string& o) { return o == "a" ? 0 : 1; };
    KyubuId ia = 0, ib = 0;
    REQUIRE(h.hook_function("a", (uintptr_t)&target_add, (void*)&hook_a, 0, (void**)&next_a, &ia) == KY_OK);
    REQUIRE(h.hook_function("b", (uintptr_t)&target_add, (void*)&hook_b, 10, (void**)&next_b, &ib) == KY_OK);
    trace.clear();
    target_add(1, 1);
    CHECK(trace == "ba");
    h.remove_all("a");
    h.remove_all("b");
    CHECK(target_add(1, 1) == 2);
}

// ---- mid hooks on hand-assembled code
static uint8_t* make_code(std::initializer_list<uint8_t> bytes) {
    auto p = (uint8_t*)VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    std::copy(bytes.begin(), bytes.end(), p);
    return p;
}

TEST_CASE("hook-framework: changing a register mid-function") {
    // covers: hook-framework/Changing a register mid-function
    // f(x) = x + 1 + 2 ; site at "add eax,1"
    uint8_t* f = make_code({0x8B, 0x44, 0x24, 0x04, 0x83, 0xC0, 0x01, 0x83, 0xC0, 0x02, 0xC3});
    auto fn = (int(__cdecl*)(int))f;
    REQUIRE(fn(10) == 13);
    int calls = 0;
    KyubuId id = 0;
    REQUIRE(Hooks::get().hook_mid("m", (uintptr_t)(f + 4), KY_SIDE_NONE,
                                  [&](KyubuCpuContext* c) { ++calls; c->eax = 100; }, &id) == KY_OK);
    CHECK(fn(10) == 103);   // handler set EAX, then the displaced "add eax,1; add eax,2" ran once
    CHECK(calls == 1);
    REQUIRE(Hooks::get().remove("m", id) == KY_OK);
    CHECK(fn(10) == 13);
}

TEST_CASE("hook-framework: mid hook context shows stack pointer and site") {
    uint8_t* f = make_code({0x8B, 0x44, 0x24, 0x04, 0x83, 0xC0, 0x01, 0x83, 0xC0, 0x02, 0xC3});
    auto fn = (int(__cdecl*)(int))f;
    uint32_t seen_arg = 0, seen_eip = 0;
    KyubuId id = 0;
    REQUIRE(Hooks::get().hook_mid("m", (uintptr_t)f, KY_SIDE_NONE,
                                  [&](KyubuCpuContext* c) { seen_arg = *(uint32_t*)(c->esp + 4); seen_eip = c->eip; },
                                  &id) == KY_OK);
    CHECK(fn(77) == 80);
    CHECK(seen_arg == 77);
    CHECK(seen_eip == (uint32_t)(uintptr_t)f);
    Hooks::get().remove("m", id);
}

TEST_CASE("hook-framework: a mid hook reads and writes the XMM registers") {
    // f(x) = int(x + x); xmm0 and xmm1 both hold x at the site, "addss xmm0,xmm1"
    uint8_t* f = make_code({0xF3, 0x0F, 0x10, 0x44, 0x24, 0x04,    // movss xmm0,[esp+4]
                            0xF3, 0x0F, 0x10, 0x4C, 0x24, 0x04,    // movss xmm1,[esp+4]
                            0xF3, 0x0F, 0x58, 0xC1,                // addss xmm0,xmm1   (site)
                            0xF3, 0x0F, 0x2C, 0xC0,                // cvttss2si eax,xmm0
                            0xC3});
    auto fn = (int(__cdecl*)(float))f;
    REQUIRE(fn(3.0f) == 6);
    float seen0 = 0, seen1 = 0;
    uint32_t seen_bits = 0;
    KyubuId id = 0;
    REQUIRE(Hooks::get().hook_mid("m", (uintptr_t)(f + 12), KY_SIDE_NONE,
                                  [&](KyubuCpuContext* c) {
                                      seen0 = c->xmm[0].f32[0];
                                      seen1 = c->xmm[1].f32[0];
                                      seen_bits = c->xmm[1].u32[0];
                                  }, &id) == KY_OK);
    CHECK(fn(3.0f) == 6);       // a handler that edits nothing leaves every XMM register as it was
    CHECK(seen0 == 3.0f);
    CHECK(seen1 == 3.0f);
    CHECK(seen_bits == 0x40400000u);
    Hooks::get().remove("m", id);
    REQUIRE(Hooks::get().hook_mid("m", (uintptr_t)(f + 12), KY_SIDE_NONE,
                                  [&](KyubuCpuContext* c) { c->xmm[0].f32[0] = 1.5f; c->xmm[1].f32[0] = 7.0f; }, &id) == KY_OK);
    CHECK(fn(3.0f) == 8);       // the edits reach the displaced "addss xmm0,xmm1": 1.5 + 7
    Hooks::get().remove("m", id);
    CHECK(fn(3.0f) == 6);
}

TEST_CASE("hook-framework: a mid hook keeps the flags of the site") {
    // cmp eax,5 sets ZF; the site is the "jne" after it, which a mid hook cannot displace, so the site is a mov that
    // leaves the flags alone: mov ecx,1 (5 bytes); then sete al reads ZF
    uint8_t* f = make_code({0x31, 0xC0, 0x83, 0xF8, 0x00,           // xor eax,eax; cmp eax,0 (ZF = 1)
                            0xB9, 0x01, 0x00, 0x00, 0x00,           // mov ecx,1   (site, 5 bytes)
                            0x0F, 0x94, 0xC0,                        // sete al
                            0x0F, 0xB6, 0xC0,                        // movzx eax,al
                            0xC3});
    auto fn = (int(__cdecl*)())f;
    REQUIRE(fn() == 1);
    KyubuId id = 0;
    uint32_t flags = 0;
    REQUIRE(Hooks::get().hook_mid("m", (uintptr_t)(f + 5), KY_SIDE_NONE,
                                  [&](KyubuCpuContext* c) { flags = c->eflags; }, &id) == KY_OK);
    CHECK(fn() == 1);
    CHECK((flags & 0x40) != 0);   // ZF as the site saw it
    Hooks::get().remove("m", id);
}

TEST_CASE("hook-framework: unsafe site") {
    // covers: hook-framework/Unsafe site
    uint8_t* f = make_code({0xE8, 0, 0, 0, 0, 0x58, 0xC3});   // call $+5; pop eax; ret
    uint8_t before[7];
    memcpy(before, f, 7);
    std::string err;
    KyubuId id = 0;
    CHECK(Hooks::get().hook_mid("m", (uintptr_t)f, KY_SIDE_NONE, [](KyubuCpuContext*) {}, &id, &err) == KY_E_UNSAFE);
    CHECK(err.find("unsafe hook site") != std::string::npos);
    CHECK(memcmp(before, f, 7) == 0);
}

TEST_CASE("hook-framework: overlapping patches") {
    // covers: hook-framework/Overlapping patches
    uint8_t* f = make_code({0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0xC3});
    uint8_t ones[4] = {1, 1, 1, 1}, twos[4] = {2, 2, 2, 2};
    KyubuId pa = 0, pb = 0;
    std::string err;
    REQUIRE(Hooks::get().patch("a", (uintptr_t)f, ones, 4, &pa) == KY_OK);
    CHECK(Hooks::get().patch("b", (uintptr_t)f + 2, twos, 4, &pb, &err) == KY_E_CONFLICT);
    CHECK(err.find("a (") != std::string::npos);
    CHECK(f[2] == 1);
    REQUIRE(Hooks::get().remove("a", pa) == KY_OK);
    CHECK(f[0] == 0x90);
    CHECK(Hooks::get().patch("b", (uintptr_t)f + 2, twos, 4, &pb) == KY_OK);
    Hooks::get().remove("b", pb);
}

// ---- IAT hooks (this test executable imports kernel32!GetTickCount)
static DWORD (WINAPI* next_tick)() = nullptr;
static DWORD WINAPI fake_tick() { return 42; }

TEST_CASE("hook-framework: import hooks") {
    volatile auto real = &GetTickCount;   // force the import
    (void)real;
    KyubuId id = 0;
    std::string err;
    REQUIRE(Hooks::get().hook_import("t", nullptr, "KERNEL32.dll", "GetTickCount", (void*)&fake_tick, 0,
                                     (void**)&next_tick, &id, &err) == KY_OK);
    CHECK(GetTickCount() == 42);
    CHECK(next_tick != nullptr);
    Hooks::get().remove("t", id);
    CHECK(GetTickCount() != 42);
}

// ---- vtable hooks
struct Widget { virtual int value() { return 1; } virtual ~Widget() = default; };
// Virtual methods are __thiscall; the portable way to write a handler is __fastcall with an unused EDX.
static int (__fastcall* next_value)(Widget*, void*) = nullptr;
static void* Widget_value_original = nullptr;   // set by the first vtable test
static int __fastcall hooked_value(Widget* w, void* edx) { return next_value(w, edx) + 10; }

TEST_CASE("hook-framework: vtable hooks chain") {
    Widget* w = new Widget();
    KyubuId id = 0;
    Widget_value_original = (*(void***)w)[0];
    REQUIRE(Hooks::get().hook_vtable("v", w, 0, (void*)&hooked_value, 0, (void**)&next_value, &id) == KY_OK);
    CHECK(w->value() == 11);
    Hooks::get().remove("v", id);
    CHECK(w->value() == 1);
    delete w;
}

// d3d9 on Windows rewrites a device's vtable slots during Reset(); the hooks must come back.
static int __fastcall replacement_value(Widget*, void*) { return 5; }

TEST_CASE("hook-framework: vtable hooks are re-applied after the slot is overwritten") {
    Widget* w = new Widget();
    KyubuId id = 0;
    REQUIRE(Hooks::get().hook_vtable("v", w, 0, (void*)&hooked_value, 0, (void**)&next_value, &id) == KY_OK);
    CHECK(w->value() == 11);
    void** slot = *(void***)w;
    // Same module overwrites the slot (like d3d9 does): adopted as the new original and re-hooked.
    void* repl = (void*)&replacement_value;
    REQUIRE(safe_write((uintptr_t)slot, &repl, sizeof repl));
    CHECK(w->value() == 5);                       // our hook is gone
    CHECK(Hooks::get().repair_vtables() == 1);
    CHECK(w->value() == 15);                      // hook back, now chaining to the replacement
    CHECK(Hooks::get().repair_vtables() == 0);    // nothing left to repair
    // Another module takes the slot (an overlay tool): left alone.
    void* foreign = (void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetCurrentProcessId");
    void* before = nullptr;
    safe_read((uintptr_t)slot, &before, sizeof before);
    REQUIRE(safe_write((uintptr_t)slot, &foreign, sizeof foreign));
    CHECK(Hooks::get().repair_vtables() == 0);
    void* now = nullptr;
    safe_read((uintptr_t)slot, &now, sizeof now);
    CHECK(now == foreign);
    REQUIRE(safe_write((uintptr_t)slot, &before, sizeof before));   // put our hook back so removal restores cleanly
    Hooks::get().remove("v", id);
    CHECK(w->value() == 5);                       // removal restores the adopted original
    void* orig = Widget_value_original;
    REQUIRE(safe_write((uintptr_t)slot, &orig, sizeof orig));
    CHECK(w->value() == 1);
    delete w;
}

static volatile long g_stress_bad = 0;
static DWORD WINAPI stress_thread(LPVOID p) {
    auto s = (std::pair<volatile bool*, volatile long*>*)p;
    while (!*s->first) {
        if (target_mul(3, 4) != 12) InterlockedIncrement(&g_stress_bad);
        InterlockedExchange(s->second, 1);   // "ran at least once" (a counter would overflow in a long run)
    }
    return 0;
}

TEST_CASE("hook-framework: stress install/uninstall while another thread runs the target") {
    volatile bool stop = false;
    volatile long ran = 0;
    HANDLE t = CreateThread(nullptr, 0, &stress_thread, new std::pair<volatile bool*, volatile long*>(&stop, &ran), 0, nullptr);
    for (int i = 0; i < 500 && ran == 0; ++i) Sleep(10);   // let the worker start
    static int (__cdecl* next_m)(int, int) = nullptr;
    auto hm = +[](int a, int b) -> int { return next_m(a, b); };
    const char* env = getenv("KYUBU_STRESS_ITERATIONS");
    // Each install/uninstall suspends all threads through a system-wide thread snapshot, which costs
    // ~40 ms on real Windows (2,000 cycles = ~3 min, 10,000 = ~1 h). The default keeps a full run short.
    int iterations = env ? atoi(env) : 500;
    for (int i = 0; i < iterations; ++i) {
        KyubuId id = 0;
        REQUIRE(Hooks::get().hook_function("s", (uintptr_t)&target_mul, (void*)hm, 0, (void**)&next_m, &id) == KY_OK);
        REQUIRE(Hooks::get().remove("s", id) == KY_OK);
    }
    stop = true;
    WaitForSingleObject(t, 5000);
    CHECK(ran == 1);
    CHECK(g_stress_bad == 0);   // the target always returned the right value while being hooked/unhooked
    CHECK(target_mul(3, 4) == 12);
}

// ---- guards
TEST_CASE("native-mod-api: guarded callbacks catch faults") {
    // covers: native-mod-api/Faulting event handler (engine part)
    REQUIRE(guard_init());
    DWORD code = guarded("m", [] { *(volatile int*)0 = 1; });
    CHECK(code == EXCEPTION_ACCESS_VIOLATION);
    code = guarded("m", [] { RaiseException(0xE06D7363, EXCEPTION_NONCONTINUABLE, 0, nullptr); });
    CHECK(code == 0xE06D7363);
    int after = 0;
    code = guarded("m", [&] { after = 1; });
    CHECK(code == 0);
    CHECK(after == 1);
    // nested: inner fault does not disturb the outer guard
    int outer_after = 0;
    code = guarded("outer", [&] {
        DWORD inner = guarded("inner", [] { volatile int z = 0; volatile int r = 100 / z; (void)r; });
        CHECK_MESSAGE(inner == EXCEPTION_INT_DIVIDE_BY_ZERO, inner);
        CHECK(std::string(current_guard_owner()) == "outer");
        outer_after = 1;
    });
    CHECK(code == 0);
    CHECK(outer_after == 1);
    CHECK(current_guard_owner() == nullptr);
}

// Real Windows returns from RtlUnwind through NtContinue with EBX/ESI/EDI zeroed (Wine preserves them).
// This shim reproduces that under Wine, so the guard must not rely on those registers after unwinding.
static void* make_clobbering_unwind(void* real) {
    auto p = (uint8_t*)VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    std::vector<uint8_t> c = {
        0xFF, 0x74, 0x24, 0x10,   // push [esp+16]  ReturnValue
        0xFF, 0x74, 0x24, 0x10,   // push [esp+16]  ExceptionRecord
        0xFF, 0x74, 0x24, 0x10,   // push [esp+16]  TargetIp
        0xFF, 0x74, 0x24, 0x10,   // push [esp+16]  TargetFrame
        0xB8, 0, 0, 0, 0,         // mov eax, real
        0xFF, 0xD0,               // call eax
        0x31, 0xDB, 0x31, 0xF6, 0x31, 0xFF,   // xor ebx,ebx; xor esi,esi; xor edi,edi
        0xC2, 0x10, 0x00,         // ret 16
    };
    uint32_t r = (uint32_t)(uintptr_t)real;
    memcpy(&c[17], &r, 4);
    memcpy(p, c.data(), c.size());
    return p;
}

TEST_CASE("native-mod-api: guard survives an RtlUnwind that clobbers EBX/ESI/EDI (Windows behaviour)") {
    REQUIRE(guard_init());
    void* real = guard_swap_unwind_for_tests(nullptr);
    guard_swap_unwind_for_tests(make_clobbering_unwind(real));
    DWORD code = guarded("m", [] { *(volatile int*)0 = 1; });
    CHECK(code == EXCEPTION_ACCESS_VIOLATION);
    uint32_t v = 0;
    CHECK_FALSE(safe_read(0x10, &v, 4));
    int outer_after = 0;
    code = guarded("outer", [&] {
        DWORD inner = guarded("inner", [] { *(volatile int*)8 = 2; });
        CHECK(inner == EXCEPTION_ACCESS_VIOLATION);
        outer_after = 1;
    });
    CHECK(code == 0);
    CHECK(outer_after == 1);
    guard_swap_unwind_for_tests(real);
}

TEST_CASE("native-mod-api: guarded read of an invalid address") {
    // covers: native-mod-api/Guarded read of an invalid address
    uint32_t v = 0;
    CHECK_FALSE(safe_read(0x10, &v, 4));
    int x = 7;
    CHECK(safe_read((uintptr_t)&x, &v, 4));
    CHECK(v == 7);
}
