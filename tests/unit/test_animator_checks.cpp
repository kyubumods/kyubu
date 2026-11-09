// Unit tests of the Animator's checks panel (openspec add-ability-animation task 6.7): the validator's findings, the clip that is
// longer than the ability's length, and the part the rig has no node for. No game, no window.
#include "doctest.h"

#include <string>

#include "checks.h"
#include "rig.h"

using namespace kanimator;

namespace {

const char* kGood =
    "duration = 3000\n"
    "\n"
    "[[track]]\n"
    "part = \"hand_r\"\n"
    "channel = \"rot\"\n"
    "mode = \"add\"\n"
    "keys = [\n"
    "  { t = 0.0, v = [0, 0, 0] },\n"
    "  { t = 1.0, v = [10, 0, 0] },\n"
    "]\n";

Rig tiny_rig() {
    RigLoad r = parse_rig(
        "name = \"tiny rig\"\n"
        "[[node]]\n"
        "name = \"root\"\n"
        "part = \"root\"\n"
        "model = \"box 4 4 4\"\n"
        "[[node]]\n"
        "name = \"head\"\n"
        "parent = \"root\"\n"
        "part = \"head\"\n"
        "model = \"box 4 4 4\"\n",
        "tiny.rig.toml");
    REQUIRE(r.ok);
    return r.rig;
}

const Check* find_code(const std::vector<Check>& v, const char* code) {
    for (const Check& c : v)
        if (c.code == code) return &c;
    return nullptr;
}

}  // namespace

TEST_CASE("a good clip with no ability length and no rig has no findings") {
    CHECK(run_checks(kGood, "burst", 0, nullptr).empty());
}

TEST_CASE("the validator's errors come through with the line and the field path") {
    std::string bad = kGood;
    bad.replace(bad.find("hand_r"), 6, "hand_x");
    std::vector<Check> c = run_checks(bad, "burst", 1000, nullptr);
    REQUIRE(c.size() == 1);
    CHECK(c[0].severity == Severity::Error);
    CHECK(c[0].code == "unknown_part");
    CHECK(c[0].line == 4);
    CHECK(c[0].path == "track[0].part");
    CHECK(!c[0].hint.empty());
    CHECK(format_check(c[0]).find("line 4: track[0].part: ") == 0);
    CHECK(count_checks(c, Severity::Error) == 1);
    CHECK(count_checks(c, Severity::Warning) == 0);
}

TEST_CASE("a syntax error is a finding too, not a crash") {
    std::vector<Check> c = run_checks("duration = [\n", "burst", 1000, nullptr);
    REQUIRE(!c.empty());
    CHECK(c[0].severity == Severity::Error);
    CHECK(c[0].code == "toml_syntax");
}

TEST_CASE("a clip of 3000 ms against an ability of 1000 ms warns and names both numbers") {
    std::vector<Check> c = run_checks(kGood, "burst", 1000, nullptr);
    REQUIRE(c.size() == 1);
    CHECK(c[0].severity == Severity::Warning);
    CHECK(c[0].code == "longer_than_ability");
    CHECK(c[0].line == 1);
    CHECK(c[0].path == "duration");
    CHECK(c[0].message.find("3000 ms") != std::string::npos);
    CHECK(c[0].message.find("1000 ms") != std::string::npos);
    CHECK(c[0].message.find("2000 ms") != std::string::npos);   // what is never drawn
}

TEST_CASE("no length check when the clip fits, follows the ability, or no length was typed") {
    CHECK(run_checks(kGood, "burst", 3000, nullptr).empty());     // equal is not longer
    CHECK(run_checks(kGood, "burst", 5000, nullptr).empty());
    CHECK(run_checks(kGood, "burst", 0, nullptr).empty());        // nothing typed
    std::string follows = kGood;
    follows.replace(follows.find("3000"), 4, "\"ability\"");
    CHECK(run_checks(follows, "burst", 1000, nullptr).empty());
}

TEST_CASE("the length check reads the line of duration when other lines come first") {
    std::string text = "# a comment: duration = 1\n\nduration = 3000\n";
    text += std::string(kGood).substr(std::string("duration = 3000\n").size());
    std::vector<Check> c = run_checks(text, "burst", 1000, nullptr);
    const Check* w = find_code(c, "longer_than_ability");
    REQUIRE(w);
    CHECK(w->line == 3);
}

TEST_CASE("a part the rig has no node for is a warning, once per part") {
    Rig rig = tiny_rig();
    std::string text = kGood;   // hand_r: the tiny rig has root and head only
    text += "\n[[track]]\npart = \"hand_r\"\nchannel = \"pos\"\nmode = \"add\"\nkeys = [\n  { t = 0.0, v = [0, 0, 0] },\n"
            "  { t = 1.0, v = [0, 1, 0] },\n]\n";
    std::vector<Check> c = run_checks(text, "burst", 0, &rig);
    REQUIRE(c.size() == 1);
    CHECK(c[0].severity == Severity::Warning);
    CHECK(c[0].code == "rig_lacks_part");
    CHECK(c[0].path == "track[0].part");
    CHECK(c[0].line == 3);
    CHECK(c[0].message.find("hand_r") != std::string::npos);
    CHECK(c[0].message.find("tiny rig") != std::string::npos);
}

TEST_CASE("a part the rig has raises nothing, and both warnings can stand together") {
    Rig rig = tiny_rig();
    std::string head = kGood;
    head.replace(head.find("hand_r"), 6, "head");
    CHECK(run_checks(head, "burst", 0, &rig).empty());

    std::vector<Check> both = run_checks(kGood, "burst", 1000, &rig);
    REQUIRE(both.size() == 2);
    CHECK(both[0].code == "longer_than_ability");
    CHECK(both[1].code == "rig_lacks_part");
    CHECK(count_checks(both, Severity::Warning) == 2);
}

TEST_CASE("a clip with errors gets no warnings of the Animator's own") {
    Rig rig = tiny_rig();
    std::string bad = kGood;
    bad.replace(bad.find("add"), 3, "mul");
    std::vector<Check> c = run_checks(bad, "burst", 1000, &rig);
    REQUIRE(!c.empty());
    for (const Check& x : c) CHECK(x.severity == Severity::Error);
}
