#include "doctest.h"

#include "symbols.h"
#include "log.h"

using namespace kyubu;

#ifdef _MSC_VER
#define NOINLINE __declspec(noinline)
#else
#define NOINLINE __attribute__((noinline))
#endif

extern "C" NOINLINE int __cdecl sym_target(int v) { volatile int x = v; return x * 3; }

static std::string code_sig(uintptr_t a, int n) {
    std::string s;
    for (int i = 0; i < n; ++i) s += fmt("%02X ", ((uint8_t*)a)[i]);
    return s;
}

TEST_CASE("hook-framework: symbol maps") {
    auto& t = SymbolTable::get();
    uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
    t.set_image(base, base);   // test exe: treat current base as preferred
    uint32_t rva = (uint32_t)((uintptr_t)&sym_target - base);
    std::string good = code_sig((uintptr_t)&sym_target, 24);
    std::string text = fmt(R"(build = "test-build"
[[symbol]]
name = "Test::target"
rva = %u
signature = "%s"
convention = "cdecl"
status = "verified"

[[symbol]]
name = "Test::bad"
rva = %u
signature = "CC CC CC CC CC CC CC CC"
status = "candidate"

[[symbol]]
name = "Test::scan"
signature = "%s"
status = "verified"
)", rva, good.c_str(), rva, good.c_str());
    std::vector<std::string> errors;
    CHECK(t.load_text(text, "test.toml", "test-build", "kyubu", errors) == 3);
    CHECK(errors.empty());

    uintptr_t a = 0;
    CHECK(t.resolve("Test::target", &a) == KY_OK);
    CHECK(a == (uintptr_t)&sym_target);

    std::string why;
    // covers: hook-framework/Wrong RVA
    CHECK(t.resolve("Test::bad", &a, &why) == KY_E_UNAVAILABLE);
    CHECK(why.find("signature mismatch") != std::string::npos);

    // signature-only symbol resolves by unique scan
    CHECK_MESSAGE(t.resolve("Test::scan", &a, &why) == KY_OK, why);
    CHECK(a == (uintptr_t)&sym_target);

    CHECK(t.resolve("Nope", &a) == KY_E_NOT_FOUND);

    // duplicates keep the first definition
    std::vector<std::string> e2;
    t.load_text(text, "again.toml", "test-build", "kyubu", e2);
    CHECK(e2.size() == 3);
    CHECK(e2[0].find("duplicate symbol") != std::string::npos);
    // other builds are ignored
    std::vector<std::string> e3;
    CHECK(t.load_text(text, "x.toml", "other-build", "kyubu", e3) == 0);

    // malformed maps are reported, not loaded
    std::vector<std::string> e4;
    CHECK(t.load_text("build = \"test-build\"\n[[symbol]\nname = ", "broken.toml", "test-build", "kyubu", e4) == 0);
    CHECK_FALSE(e4.empty());
    std::vector<std::string> e5;
    CHECK(t.load_text("build = \"test-build\"\n[[symbol]]\nname = \"Test::badsig\"\nsignature = \"55 8BX\"\n", "badsig.toml",
                      "test-build", "kyubu", e5) == 0);
    CHECK_FALSE(e5.empty());

    // scanning: no match and more than one match are both "unavailable", with the reason
    std::vector<std::string> e6;
    t.load_text(R"(build = "test-build"
[[symbol]]
name = "Test::nomatch"
signature = "F1 F2 F3 F4 F5 F6 F7 F8 F9 FA FB FC"

[[symbol]]
name = "Test::many"
signature = "C3"
)", "scan.toml", "test-build", "kyubu", e6);   // "ret": in any compiler's code more than once
    CHECK(t.resolve("Test::nomatch", &a, &why) == KY_E_UNAVAILABLE);
    CHECK(why.find("not found") != std::string::npos);
    CHECK(t.resolve("Test::many", &a, &why) == KY_E_UNAVAILABLE);
    CHECK(why.find("more than once") != std::string::npos);
}

TEST_CASE("hook-framework: mod adds a symbol") {
    // covers: hook-framework/Mod adds a symbol
    auto& t = SymbolTable::get();
    uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
    t.set_image(base, base);   // do not depend on another test case having set the image
    uint32_t rva = (uint32_t)((uintptr_t)&sym_target - base);
    std::vector<std::string> errors;
    std::string text = fmt("build = \"test-build\"\n[[symbol]]\nname = \"re_items:ItemModelResolver\"\nrva = %u\n", rva);
    CHECK(t.load_text(text, "re.toml", "test-build", "re_items", errors) == 1);
    uintptr_t a = 0;
    CHECK(t.resolve("re_items:ItemModelResolver", &a) == KY_OK);
    CHECK(a == (uintptr_t)&sym_target);
    std::string bad = fmt("build = \"test-build\"\n[[symbol]]\nname = \"Unprefixed\"\nrva = %u\n", rva);
    CHECK(t.load_text(bad, "re2.toml", "test-build", "re_items", errors) == 0);
    CHECK(errors.back().find("must be named 're_items:") != std::string::npos);
}

static int sym_global_var = 7;   // data, outside .text

TEST_CASE("hook-framework: global symbols") {
    auto& t = SymbolTable::get();
    uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
    t.set_image(base, base);
    uint32_t rva = (uint32_t)((uintptr_t)&sym_global_var - base);
    std::vector<std::string> errors;
    std::string text = fmt(R"(build = "test-build"
[[global]]
name = "Test::global_var"
address = %u
type = "int32_t"
status = "verified"

[[symbol]]
name = "Test::data_as_function"
address = %u
status = "candidate"

[[symbol]]
name = "Test::global_outside"
address = %u
kind = "global"
status = "verified"

[[symbol]]
name = "Test::global_by_signature"
signature = "55 8B EC"
kind = "global"
)", base + rva, base + rva, base + 0x7FFFFFF0u);
    CHECK(t.load_text(text, "globals.toml", "test-build", "kyubu", errors) == 3);
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].find("needs rva/address") != std::string::npos);   // a global has no signature to scan for
    uintptr_t a = 0;
    std::string why;
    CHECK_MESSAGE(t.resolve("Test::global_var", &a, &why) == KY_OK, why);   // not in .text, but a global
    CHECK(a == (uintptr_t)&sym_global_var);
    CHECK(t.find("Test::global_var")->global);
    CHECK(t.resolve("Test::data_as_function", &a, &why) == KY_E_UNAVAILABLE);   // a function must be in .text
    CHECK(why.find("outside the code section") != std::string::npos);
    CHECK(t.resolve("Test::global_outside", &a, &why) == KY_E_UNAVAILABLE);
    CHECK(why.find("outside the image") != std::string::npos);
}

TEST_CASE("hook-framework: signature parsing") {
    bool ok;
    auto s = SymbolTable::parse_signature("55 8B ?? EC ?", &ok);
    CHECK(ok);
    CHECK(s == std::vector<int>{0x55, 0x8B, -1, 0xEC, -1});
    SymbolTable::parse_signature("55 8BX", &ok);
    CHECK_FALSE(ok);
}
