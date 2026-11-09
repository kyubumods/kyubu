// Unit tests of the words of `anim preview` and of its replies (openspec add-ability-animation, task 7.1; mods-dev/anim_capture). No game.
#include "doctest.h"

#include <string>

#include "preview_args.h"

using namespace kyubu::anim_capture;

TEST_CASE("anim preview: a clip and a time") {
    const PreviewArgs p = parse_preview_args("demo:slam 240");
    REQUIRE(p.kind == PreviewArgs::Kind::Show);
    CHECK(p.clip == "demo:slam");
    CHECK(p.t_ms == 240.0);

    const PreviewArgs spaced = parse_preview_args("  demo:slam \t 0.5  ");
    REQUIRE(spaced.kind == PreviewArgs::Kind::Show);
    CHECK(spaced.t_ms == 0.5);
    CHECK(parse_preview_args("demo:slam 0").kind == PreviewArgs::Kind::Show);
}

TEST_CASE("anim preview: off") {
    CHECK(parse_preview_args("off").kind == PreviewArgs::Kind::Off);
    CHECK(parse_preview_args("  off ").kind == PreviewArgs::Kind::Off);
    // "off" and a time is a clip named off, which the loader will not find; it is not the off word
    CHECK(parse_preview_args("off 10").kind == PreviewArgs::Kind::Show);
}

TEST_CASE("anim preview: wrong input gives the usage or the bad time") {
    for (const char* bad : {"", "   ", "demo:slam", "demo:slam 1 2", "on"}) {
        const PreviewArgs p = parse_preview_args(bad);
        REQUIRE_MESSAGE(p.kind == PreviewArgs::Kind::Error, bad);
        CHECK_MESSAGE(p.error == kPreviewUsage, bad);
    }
    for (const char* bad : {"demo:slam abc", "demo:slam -5", "demo:slam 12ms", "demo:slam nan", "demo:slam inf", "demo:slam 1e999"}) {
        const PreviewArgs p = parse_preview_args(bad);
        REQUIRE_MESSAGE(p.kind == PreviewArgs::Kind::Error, bad);
        CHECK_MESSAGE(p.error.find("is not a time") != std::string::npos, bad);
    }
}

TEST_CASE("anim preview: the reply names what happened") {
    const PreviewArgs show = parse_preview_args("demo:slam 240");
    CHECK(preview_reply(show, KY_OK) == "anim preview: demo:slam at 240 ms on the local player (`anim preview off` ends it)");
    CHECK(preview_reply(show, KY_E_NOT_FOUND) == "anim preview: no clip 'demo:slam' (anim list shows the loaded clips)");
    CHECK(preview_reply(show, KY_E_DEV_ONLY).find("dev mode") != std::string::npos);
    CHECK(preview_reply(show, KY_E_TRUST).find("trust") != std::string::npos);
    CHECK(preview_reply(show, KY_E_INTERNAL).find("result -13") != std::string::npos);

    const PreviewArgs off = parse_preview_args("off");
    CHECK(preview_reply(off, KY_OK) == "anim preview: off, the game draws the player again");
    CHECK(preview_reply(off, KY_E_TRUST).find("failed") != std::string::npos);

    const PreviewArgs bad = parse_preview_args("");
    CHECK(preview_reply(bad, KY_OK) == kPreviewUsage);
}
