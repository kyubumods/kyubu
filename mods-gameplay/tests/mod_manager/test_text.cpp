// Task 5.3: text::fit cuts a string to a width with "..." (the font itself is only checked in the game).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "text.h"

namespace {
float w(const std::wstring& s) { return 10.0f * static_cast<float>(s.size()); }   // 10 px a character
}  // namespace

TEST_CASE("fit leaves a string that fits") {
    CHECK(text::fit(L"ABCDE", 50.0f, w) == L"ABCDE");
    CHECK(text::fit(L"", 5.0f, w) == L"");
}

TEST_CASE("fit cuts to the longest prefix with dots that fits") {
    CHECK(text::fit(L"ABCDEFGH", 70.0f, w) == L"ABCD...");
    CHECK(text::fit(L"ABCDEFGH", 40.0f, w) == L"A...");
    CHECK(w(text::fit(L"ABCDEFGHIJ", 65.0f, w)) <= 65.0f);
}

TEST_CASE("fit gives an empty string when even the dots do not fit") {
    CHECK(text::fit(L"ABCDEFGH", 20.0f, w) == L"");
    CHECK(text::fit(L"ABCDEFGH", 30.0f, w) == L"...");
}

TEST_CASE("GameString uses the buffer up to 7 characters and a pointer beyond") {
    const std::wstring a = L"ABCDEFG", b = L"ABCDEFGH";
    text::GameString ga(a.data(), a.size()), gb(b.data(), b.size());
    CHECK(ga.size == 7); CHECK(ga.capacity == 7); CHECK(ga.buf[6] == L'G');
    CHECK(gb.size == 8); CHECK(gb.ptr == b.data());
}

TEST_CASE("wrap_capped keeps all the lines when they fit the cap") {
    const std::vector<std::wstring> two = text::wrap_capped(L"AAA BBB CCC DDD", 80.0f, 2, w);
    REQUIRE(two.size() == 2);
    CHECK(two[0] == L"AAA BBB");
    CHECK(two[1] == L"CCC DDD");
    CHECK(text::wrap_capped(L"AAA BBB CCC DDD", 80.0f, 5, w).size() == 2);
}

TEST_CASE("wrap_capped ends the last line in dots when text is left over") {
    const std::vector<std::wstring> one = text::wrap_capped(L"AAA BBB CCC DDD", 80.0f, 1, w);
    REQUIRE(one.size() == 1);
    CHECK(one[0] == L"AAA B...");
    const std::vector<std::wstring> three = text::wrap_capped(L"AAA BBB CCC DDD EEE FFF GGG", 80.0f, 2, w);
    REQUIRE(three.size() == 2);
    CHECK(three[0] == L"AAA BBB");
    CHECK(three[1] == L"CCC D...");
}
