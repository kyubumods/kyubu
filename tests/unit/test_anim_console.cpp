// Unit tests of the `anim` console commands' text (openspec add-ability-animation task 3.7). No game, no console: the commands are
// pure functions of a ClipSet. The fixture clips are the ones of the kanim tests.
#include "doctest.h"

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "animation_console.h"
#include "kanim_core.h"

using namespace kyubu::game::anim;

namespace {

std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    REQUIRE_MESSAGE(f.good(), "cannot read " << path);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::string joined(const std::vector<std::string>& lines) {
    std::string s;
    for (auto& l : lines) s += l + "\n";
    return s;
}

// design_example, loop and pops under one mod; `bad` is rejected by the registry.
std::unique_ptr<ClipSet> fixture_set() {
    const std::string dir = KYUBU_SOURCE_DIR "/tests/fixtures/kanim/";
    std::vector<ClipSource> src;
    for (const char* n : {"design_example", "loop", "pops", "bad"}) {
        ClipSource s;
        s.mod_id = "fx";
        s.file = dir + n + ".anim.toml";
        s.text = slurp(s.file);
        src.push_back(s);
    }
    return build_clip_set(src, nullptr, nullptr);
}

}  // namespace

TEST_CASE("anim list: each loaded clip with its duration and tracks") {
    auto set = fixture_set();
    const auto lines = anim_command_lines(*set, "list");
    REQUIRE(lines.size() == 4);   // the header and three clips: the bad one is not loaded
    CHECK(lines[0].find("3 clip(s)") == 0);
    CHECK(lines[0].find(set->hash.substr(0, 12)) != std::string::npos);
    CHECK(lines[1].find("  fx:design_example  ") == 0);
    CHECK(lines[1].find("  ability  ") != std::string::npos);   // duration = "ability"
    CHECK(lines[1].find(" track(s)  ") != std::string::npos);
    CHECK(lines[2].find("  fx:loop  ") == 0);
    CHECK(lines[2].find("  500 ms  ") != std::string::npos);
    CHECK(lines[3].find("  fx:pops  ") == 0);
    CHECK(joined(lines).find("fx:bad") == std::string::npos);
    // surrounding space does not matter
    CHECK(anim_command_lines(*set, "  list  ") == lines);
}

TEST_CASE("anim list: an empty set says what to do") {
    ClipSet empty;
    const auto lines = anim_command_lines(empty, "list");
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("no clips") == 0);
}

TEST_CASE("anim show: the text of kanim describe") {
    auto set = fixture_set();
    const std::string id = "fx:design_example";
    const auto lines = anim_command_lines(*set, "show " + id);
    kyubu::kanim::Options o;
    const std::string want = kyubu::kanim::describe(set->find(id)->text, id, o).text;
    CHECK(joined(lines) == want);
    CHECK(lines.size() > 3);
}

TEST_CASE("anim sample: the text of kanim sample, at the default and at given fractions") {
    auto set = fixture_set();
    const std::string id = "fx:loop";
    kyubu::kanim::Options o;
    CHECK(joined(anim_command_lines(*set, "sample " + id)) == kyubu::kanim::sample(set->find(id)->text, id, o).text);
    o.times = {0.25f, 1.0f};
    CHECK(joined(anim_command_lines(*set, "sample " + id + " 0.25,1")) == kyubu::kanim::sample(set->find(id)->text, id, o).text);
}

TEST_CASE("anim: wrong input gives one helpful line") {
    auto set = fixture_set();
    for (const char* args : {"", "bogus", "show", "sample", "list extra", "show a b"}) {
        const auto lines = anim_command_lines(*set, args);
        REQUIRE_MESSAGE(lines.size() == 1, args);
        CHECK_MESSAGE(lines[0] == kAnimUsage, args);
    }
    auto unknown = anim_command_lines(*set, "show fx:missing");
    REQUIRE(unknown.size() == 1);
    CHECK(unknown[0].find("no clip 'fx:missing'") == 0);
    auto bad_t = anim_command_lines(*set, "sample fx:loop 2");
    REQUIRE(bad_t.size() == 1);
    CHECK(bad_t[0].find("not a list of times") != std::string::npos);
    CHECK(anim_command_lines(*set, "show fx:bad")[0].find("no clip 'fx:bad'") == 0);
}

TEST_CASE("anim: a mod's subcommand is the command <mod id>:anim.<subcommand>") {
    const std::vector<std::string> names = {"anim", "anim_capture:anim.capture", "anim_capture:anim.preview", "teleport:tp", "other:anim.list",
                                            "other:anim.sample", "other:anim.", "x:anim.two words", "anim.preview", ":anim.zero", "a:b:anim.deep"};
    const std::vector<AnimExtension> found = anim_extensions(names);
    REQUIRE(found.size() == 2);
    CHECK(found[0].sub == "capture");
    CHECK(found[0].command == "anim_capture:anim.capture");
    CHECK(found[1].sub == "preview");
    CHECK(found[1].command == "anim_capture:anim.preview");

    CHECK(anim_extension_command(names, "preview") == "anim_capture:anim.preview");
    CHECK(anim_extension_command(names, "list") == "");    // a builtin subcommand is never handed on
    CHECK(anim_extension_command(names, "sample") == "");
    CHECK(anim_extension_command(names, "tp") == "");
    CHECK(anim_extension_command(names, "") == "");
    CHECK(anim_extension_command({}, "preview") == "");    // the capture mod is not loaded: nothing to hand on to
}

TEST_CASE("anim: the first claim of a subcommand wins") {
    const std::vector<std::string> names = {"a_mod:anim.preview", "b_mod:anim.preview"};
    CHECK(anim_extension_command(names, "preview") == "a_mod:anim.preview");
}

TEST_CASE("anim: a line is split into the subcommand and the rest") {
    std::string sub, rest;
    split_anim_args("preview demo:slam 240", sub, rest);
    CHECK(sub == "preview");
    CHECK(rest == "demo:slam 240");
    split_anim_args("  preview   off  ", sub, rest);
    CHECK(sub == "preview");
    CHECK(rest == "off");
    split_anim_args("reload", sub, rest);
    CHECK(sub == "reload");
    CHECK(rest == "");
    split_anim_args("", sub, rest);
    CHECK(sub == "");
    CHECK(rest == "");
}
