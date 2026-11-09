// Task 6.5: the set of mods that arrived by hand.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "hand.h"

TEST_CASE("the set round-trips and ignores bad lines") {
    const std::set<std::string> ids = hand::parse("b_mod\r\n\nA Bad Line\na-mod\n" + std::string(40, 'x') + "\nb_mod\n");
    CHECK(ids == std::set<std::string>{"a-mod", "b_mod"});
    CHECK(hand::write(ids) == "a-mod\nb_mod\n");
    CHECK(hand::parse(hand::write(ids)) == ids);
    CHECK(hand::parse("").empty());
}
