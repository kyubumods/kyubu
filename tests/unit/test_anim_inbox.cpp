// Unit tests of the dev-mode file inbox (openspec add-ability-animation, task 7.6): the capture mod's request parser
// (mods-dev/anim_capture/src/inbox.h) and the Animator's `In game` helpers (tools/animator/ingame.h). No game.
#include "doctest.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "inbox.h"
#include "ingame.h"
#include "preview_args.h"

using namespace kyubu::anim_capture;

TEST_CASE("inbox: a request is one console line of the capture mod's own words") {
    const InboxRequest c = parse_inbox_request("anim capture demo:slam 0,500 front\n");
    REQUIRE(c.ok);
    CHECK(c.sub == "capture");
    CHECK(c.args == "demo:slam 0,500 front");
    CHECK(c.line == "anim capture demo:slam 0,500 front");

    const InboxRequest r = parse_inbox_request("anim rig");
    REQUIRE(r.ok);
    CHECK(r.sub == "rig");
    CHECK(r.args.empty());

    for (const char* sub : {"preview", "capture", "rig", "bake"}) CHECK(parse_inbox_request(std::string("anim ") + sub + " x 1").ok);
}

TEST_CASE("inbox: a byte order mark, blank lines, CRLF and a note below the line are tolerated") {
    const InboxRequest r = parse_inbox_request("\xEF\xBB\xBF\r\n  \r\n  anim   preview  demo:slam  240 \r\nnote: from the AI\r\n");
    REQUIRE(r.ok);
    CHECK(r.sub == "preview");
    CHECK(r.args == "demo:slam  240");
    // the arguments are the preview command's own: it parses them
    const PreviewArgs p = parse_preview_args(r.args);
    REQUIRE(p.kind == PreviewArgs::Kind::Show);
    CHECK(p.clip == "demo:slam");
    CHECK(p.t_ms == 240.0);
}

TEST_CASE("inbox: any other console line is refused with a reason") {
    for (const char* bad : {"quit", "anim", "anim list", "anim sample demo:slam 0.5", "ability rank 1 1", "give 3 1", "ANIM capture x 0",
                            "kyubu_game:anim.capture x", "animx capture x"}) {
        const InboxRequest r = parse_inbox_request(bad);
        CHECK_MESSAGE(!r.ok, bad);
        CHECK_MESSAGE(r.error.rfind("inbox: ", 0) == 0, bad);
    }
    CHECK(parse_inbox_request("").error == "inbox: the request file has no console line");
    CHECK(parse_inbox_request(" \r\n\r\n").error == "inbox: the request file has no console line");
    CHECK(!parse_inbox_request(std::string("anim capture x ") + std::string(kInboxMaxBytes, '1')).ok);
    CHECK(!parse_inbox_request(std::string("anim rig\x01x")).ok);
}

TEST_CASE("inbox: request and reply names") {
    CHECK(is_inbox_request_name("capture-1.txt"));
    CHECK_FALSE(is_inbox_request_name("capture-1.reply"));
    CHECK_FALSE(is_inbox_request_name("capture-1.tmp"));
    CHECK_FALSE(is_inbox_request_name(".txt"));
    CHECK_FALSE(is_inbox_request_name(".hidden.txt"));
    CHECK(inbox_reply_name("capture-1.txt") == "capture-1.reply");
    CHECK(is_inbox_request_name("animator-0001.txt"));
}

TEST_CASE("In game: the clip's id from its path, and the line the button sends") {
    using kanimator::ingame_clip_id;
    CHECK(ingame_clip_id("D:\\x\\mods-dev\\demo\\assets\\animations\\slam.anim.toml", "slam") == "demo:slam");
    CHECK(ingame_clip_id("mods/demo/assets/animations/slam.anim.toml", "slam") == "demo:slam");
    CHECK(ingame_clip_id("/assets/animations/slam.anim.toml", "slam").empty());
    CHECK(ingame_clip_id("C:\\temp\\slam.anim.toml", "slam").empty());
    CHECK(ingame_clip_id("", "slam").empty());

    CHECK(kanimator::ingame_preview_line("demo:slam", 241.6667) == "anim preview demo:slam 241.7");
    CHECK(kanimator::ingame_preview_line("demo:slam", -3) == "anim preview demo:slam 0.0");
    // what the Animator sends is what the mod runs
    const InboxRequest r = parse_inbox_request(kanimator::ingame_preview_line("demo:slam", 120) + "\n");
    REQUIRE(r.ok);
    CHECK(r.sub == "preview");
    CHECK(parse_preview_args(r.args).kind == PreviewArgs::Kind::Show);
}

TEST_CASE("In game: the request is dropped under a temporary name and renamed; no game folder or no inbox folder says why") {
    namespace fs = std::filesystem;
    std::string why;
    CHECK_FALSE(kanimator::ingame_drop("", "anim rig", &why));
    CHECK(why.find("no game folder") != std::string::npos);

    const fs::path game = fs::temp_directory_path() / "kyubu_inbox_test";
    fs::remove_all(game);
    why.clear();
    CHECK_FALSE(kanimator::ingame_drop(game.string(), "anim rig", &why));
    CHECK(why.find("anim_capture") != std::string::npos);

    fs::create_directories(game / "kyubu" / "inbox");
    REQUIRE(kanimator::ingame_drop(game.string() + "\\", "anim preview demo:slam 100", &why));
    int txt = 0, other = 0;
    std::string text;
    for (const auto& e : fs::directory_iterator(game / "kyubu" / "inbox")) {
        if (e.path().extension() == ".txt") {
            ++txt;
            std::ifstream f(e.path(), std::ios::binary);
            text.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        } else {
            ++other;
        }
    }
    CHECK(txt == 1);
    CHECK(other == 0);
    CHECK(text == "anim preview demo:slam 100\n");
    fs::remove_all(game);
}
