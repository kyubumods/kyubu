// Unit tests of the animation editor's API calls (openspec add-ability-animation task 3.5): animation_define_from_text,
// animation_preview and animation_preview_clear, through the real API table with mods that are not loaded from disk.
#include "doctest.h"

#include <cstring>
#include <limits>
#include <string>

#include "animations.h"
#include "api.h"
#include "call.h"
#include "loader.h"
#include "log.h"
#include "modmanager.h"

using namespace kyubu;

namespace {

struct EditorMod {
    Mod mod;
    explicit EditorMod(const char* id, const char* trust = "full") {
        mod.id = id;
        mod.state = ModState::Loaded;
        mod.manifest.sides = KY_SIDE_BOTH;
        mod.manifest.trust = trust;
        for (int s = 1; s <= 2; ++s) { mod.ctx[s].mod = &mod; mod.ctx[s].side = (KyubuSide)s; }
    }
    KyubuModHandle client() { return mod.handle(KY_SIDE_CLIENT); }
    KyubuModHandle server() { return mod.handle(KY_SIDE_SERVER); }
};

// Dev mode set for the test, and the editor's state clean at both ends.
struct DevMode {
    bool saved = L().dev_mode;
    explicit DevMode(bool on = true) {
        L().dev_mode = on;
        Animations::get().clear_preview();
    }
    ~DevMode() {
        Animations::get().clear_preview();
        L().dev_mode = saved;
    }
};

std::string clip_text(int duration_ms) {
    return "duration = " + std::to_string(duration_ms) +
           "\n\n[[track]]\npart = \"hand_r\"\nchannel = \"rot\"\nkeys = [ { t = 0, v = [0, 0, 0] }, { t = 1, v = [30, 0, 0] } ]\n";
}

// A key whose `t` is 1.4.
const char* const kBadText =
    "duration = 400\n[[track]]\npart = \"hand_r\"\nchannel = \"rot\"\nkeys = [ { t = 1.4, v = [0, 0, 0] } ]\n";

const game::anim::ClipEntry* find(const char* id) { return Animations::get().clips().set().find(id); }

}  // namespace

TEST_CASE("game-api: animation_define_from_text publishes a clip with no file") {
    // covers: game-api/Define from text: Publish an in-memory clip
    DevMode dev;
    EditorMod ed("ed_pub");
    const KyubuApi* api = api_table();
    const std::string before = Animations::get().hash();
    char err[256] = "untouched";
    REQUIRE(api->animation_define_from_text(ed.client(), "slam", clip_text(400).c_str(), err, sizeof err) == KY_OK);
    CHECK(std::string(err).empty());
    const game::anim::ClipEntry* e = find("ed_pub:slam");
    REQUIRE(e);
    CHECK(e->clip.duration_ms == 400);
    CHECK(e->text == clip_text(400));   // `anim show` reads this
    CHECK(Animations::get().hash() != before);

    // the same name again replaces it
    REQUIRE(api->animation_define_from_text(ed.client(), "slam", clip_text(700).c_str(), nullptr, 0) == KY_OK);
    CHECK(find("ed_pub:slam")->clip.duration_ms == 700);
    CHECK(find("ed_pub:slam") != e);
    // the id is the caller's own: a second mod's "slam" is another clip
    EditorMod ed2("ed_pub2");
    REQUIRE(api->animation_define_from_text(ed2.client(), "slam", clip_text(100).c_str(), nullptr, 0) == KY_OK);
    CHECK(find("ed_pub2:slam")->clip.duration_ms == 100);
    CHECK(find("ed_pub:slam")->clip.duration_ms == 700);
}

TEST_CASE("game-api: animation_define_from_text with a bad text changes nothing and says where") {
    // covers: game-api/Define from text: A bad text
    DevMode dev;
    EditorMod ed("ed_bad");
    const KyubuApi* api = api_table();
    REQUIRE(api->animation_define_from_text(ed.client(), "keep", clip_text(250).c_str(), nullptr, 0) == KY_OK);
    const std::string hash = Animations::get().hash();
    const game::anim::ClipEntry* kept = find("ed_bad:keep");

    char err[512] = {};
    CHECK(api->animation_define_from_text(ed.client(), "keep", kBadText, err, sizeof err) == KY_E_INVALID_ARG);
    CHECK(std::string(err).find("keys[0].t") != std::string::npos);
    CHECK(std::string(err).find("(editor)/keep.anim.toml:") == 0);   // the source, then the line
    CHECK(Animations::get().hash() == hash);
    CHECK(find("ed_bad:keep") == kept);
    CHECK(game::last_refusal().find("keys[0].t") != std::string::npos);   // Lua's second result

    CHECK(api->animation_define_from_text(ed.client(), "fresh", kBadText, err, sizeof err) == KY_E_INVALID_ARG);
    CHECK(find("ed_bad:fresh") == nullptr);
    CHECK(Animations::get().hash() == hash);

    // the error buffer is cut and terminated; a NULL one is fine
    char tiny[8];
    std::memset(tiny, 'x', sizeof tiny);
    CHECK(api->animation_define_from_text(ed.client(), "fresh", kBadText, tiny, sizeof tiny) == KY_E_INVALID_ARG);
    CHECK(std::strlen(tiny) == 7);
    CHECK(api->animation_define_from_text(ed.client(), "fresh", kBadText, nullptr, 0) == KY_E_INVALID_ARG);

    // a bad name, a NULL name, a NULL text
    for (const char* name : {"", "Bad", "has space", "a/b"})
        CHECK(api->animation_define_from_text(ed.client(), name, clip_text(100).c_str(), err, sizeof err) == KY_E_INVALID_ARG);
    CHECK(api->animation_define_from_text(ed.client(), std::string(65, 'a').c_str(), clip_text(100).c_str(), err, sizeof err) ==
          KY_E_INVALID_ARG);
    CHECK(std::string(err).find("1 to 64 characters") != std::string::npos);
    CHECK(api->animation_define_from_text(ed.client(), std::string(64, 'a').c_str(), clip_text(100).c_str(), err, sizeof err) == KY_OK);
    CHECK(api->animation_define_from_text(ed.client(), nullptr, "x", err, sizeof err) == KY_E_INVALID_ARG);
    CHECK(api->animation_define_from_text(ed.client(), "n", nullptr, err, sizeof err) == KY_E_INVALID_ARG);
}

TEST_CASE("game-api: a scan keeps a clip the editor defined") {
    // covers: game-api/Define from text: Publish an in-memory clip
    DevMode dev;
    EditorMod ed("ed_scan");
    const KyubuApi* api = api_table();
    REQUIRE(api->animation_define_from_text(ed.client(), "slam", clip_text(333).c_str(), nullptr, 0) == KY_OK);
    Animations::get().scan();   // reads the mods' files: this mod has none
    REQUIRE(find("ed_scan:slam"));
    CHECK(find("ed_scan:slam")->clip.duration_ms == 333);
}

TEST_CASE("game-api: the editor calls are refused outside full trust, dev mode and the client side") {
    // covers: game-api/Animation editor calls: Refused, Editor calls on the server side; animation-editor/The Animator is a dev tool: A restricted mod calls the editor's API
    const KyubuApi* api = api_table();
    char err[64] = {};
    {
        DevMode dev;
        EditorMod good("ed_ref_a");
        REQUIRE(api->animation_define_from_text(good.client(), "slam", clip_text(100).c_str(), nullptr, 0) == KY_OK);

        EditorMod restricted("ed_ref_r", "restricted");
        const std::string hash = Animations::get().hash();
        const uint64_t seq = Log::get().sequence();
        CHECK(api->animation_define_from_text(restricted.client(), "slam", clip_text(100).c_str(), err, sizeof err) == KY_E_TRUST);
        CHECK(api->animation_preview(restricted.client(), "ed_ref_a:slam", 10) == KY_E_TRUST);
        CHECK(api->animation_preview_clear(restricted.client()) == KY_E_TRUST);
        CHECK(Animations::get().hash() == hash);
        CHECK_FALSE(Animations::get().preview_on());
        bool logged = false;
        for (auto& e : Log::get().since(seq)) logged |= e.text.find("needs trust \"full\"") != std::string::npos;
        CHECK(logged);

        // the server side: refused whatever the trust
        CHECK(api->animation_define_from_text(good.server(), "slam2", clip_text(100).c_str(), err, sizeof err) == KY_E_SIDE);
        CHECK(api->animation_preview(good.server(), "ed_ref_a:slam", 10) == KY_E_SIDE);
        CHECK(api->animation_preview_clear(good.server()) == KY_E_SIDE);
        CHECK(find("ed_ref_a:slam2") == nullptr);
        CHECK_FALSE(Animations::get().preview_on());

        // a faulted mod and a handle that is not a mod's
        good.mod.state = ModState::Faulted;
        CHECK(api->animation_preview(good.client(), "ed_ref_a:slam", 10) == KY_E_FAULTED);
        good.mod.state = ModState::Loaded;
        CHECK(api->animation_preview(nullptr, "ed_ref_a:slam", 10) == KY_E_INVALID_ARG);
    }
    {
        DevMode off(false);
        EditorMod ed("ed_ref_d");
        const std::string hash = Animations::get().hash();
        CHECK(api->animation_define_from_text(ed.client(), "slam", clip_text(100).c_str(), err, sizeof err) == KY_E_DEV_ONLY);
        CHECK(api->animation_preview(ed.client(), "ed_ref_d:slam", 10) == KY_E_DEV_ONLY);
        CHECK(api->animation_preview_clear(ed.client()) == KY_E_DEV_ONLY);
        CHECK(Animations::get().hash() == hash);
        CHECK(find("ed_ref_d:slam") == nullptr);
    }
}

TEST_CASE("game-api: animation_preview sets the clock the provider reads, and clear turns it off") {
    // covers: game-api/Preview calls: Preview, Preview cleared
    DevMode dev;
    EditorMod ed("ed_prev");
    const KyubuApi* api = api_table();
    REQUIRE(api->animation_define_from_text(ed.client(), "slam", clip_text(400).c_str(), nullptr, 0) == KY_OK);

    Animations::Preview p;
    CHECK_FALSE(Animations::get().preview_on());
    CHECK_FALSE(Animations::get().preview(&p));

    REQUIRE(api->animation_preview(ed.client(), "ed_prev:slam", 240) == KY_OK);
    REQUIRE(Animations::get().preview_on());
    REQUIRE(Animations::get().preview(&p));
    CHECK(p.clip_id == "ed_prev:slam");
    CHECK(p.t_ms == 240.0f);

    // a second call moves the clock
    REQUIRE(api->animation_preview(ed.client(), "ed_prev:slam", 0) == KY_OK);
    REQUIRE(Animations::get().preview(&p));
    CHECK(p.t_ms == 0.0f);

    // a refused call leaves it as it was
    CHECK(api->animation_preview(ed.client(), "ed_prev:nope", 5) == KY_E_NOT_FOUND);
    CHECK(game::last_refusal().find("no clip 'ed_prev:nope'") == 0);
    CHECK(api->animation_preview(ed.client(), nullptr, 5) == KY_E_INVALID_ARG);
    CHECK(api->animation_preview(ed.client(), "ed_prev:slam", -1) == KY_E_INVALID_ARG);
    CHECK(api->animation_preview(ed.client(), "ed_prev:slam", std::numeric_limits<double>::quiet_NaN()) == KY_E_INVALID_ARG);
    CHECK(api->animation_preview(ed.client(), "ed_prev:slam", std::numeric_limits<double>::infinity()) == KY_E_INVALID_ARG);
    REQUIRE(Animations::get().preview(&p));
    CHECK(p.t_ms == 0.0f);

    // a time past the clip's end is for the evaluator (hold or loop), not an error
    CHECK(api->animation_preview(ed.client(), "ed_prev:slam", 90000) == KY_OK);

    CHECK(api->animation_preview_clear(ed.client()) == KY_OK);
    CHECK_FALSE(Animations::get().preview_on());
    CHECK_FALSE(Animations::get().preview(&p));
    CHECK(api->animation_preview_clear(ed.client()) == KY_OK);   // already off
}
