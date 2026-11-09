// Unit tests of the clip registry (openspec add-ability-animation task 3.3). No game, no files.
#include "doctest.h"

#include <string>
#include <thread>
#include <vector>

#include "animation_registry.h"

using namespace kyubu::game::anim;

namespace {

std::string clip_text(int duration_ms) {
    return "duration = " + std::to_string(duration_ms) +
           "\n\n[[track]]\npart = \"hand_r\"\nchannel = \"rot\"\nkeys = [ { t = 0, v = [0, 0, 0] }, { t = 1, v = [30, 0, 0] } ]\n";
}

// Line 3 has the error: a `t` above 1.
const char* const kBadText =
    "duration = 400\n[[track]]\npart = \"hand_r\"\nchannel = \"rot\"\nkeys = [ { t = 2, v = [0, 0, 0] } ]\n";

ClipSource src(const std::string& mod, const std::string& file, const std::string& text) { return {mod, file, text, false}; }

int count(const std::vector<BuildLine>& lines, BuildLine::Kind k) {
    int n = 0;
    for (auto& l : lines) n += l.kind == k;
    return n;
}

const BuildLine* first(const std::vector<BuildLine>& lines, BuildLine::Kind k) {
    for (auto& l : lines)
        if (l.kind == k) return &l;
    return nullptr;
}

}  // namespace

TEST_CASE("anim registry: the name of a clip comes from its file") {
    std::string n;
    CHECK(clip_name_from_file("slam.anim.toml", n));
    CHECK(n == "slam");
    CHECK(clip_name_from_file("mods/demo/assets/animations/heavy_slam-2.v1.anim.toml", n));
    CHECK(n == "heavy_slam-2.v1");
    CHECK(clip_name_from_file("mods\\demo\\assets\\animations\\Slam.ANIM.TOML", n));
    CHECK(n == "slam");
    CHECK(clip_name_from_file(std::string(64, 'a') + ".anim.toml", n));
    CHECK(n.size() == 64);

    n = "untouched";
    CHECK_FALSE(clip_name_from_file(".anim.toml", n));
    CHECK_FALSE(clip_name_from_file("slam.toml", n));
    CHECK_FALSE(clip_name_from_file("slam.anim.toml.bak", n));
    CHECK_FALSE(clip_name_from_file("my slam.anim.toml", n));
    CHECK_FALSE(clip_name_from_file("demo:slam.anim.toml", n));
    CHECK_FALSE(clip_name_from_file("sl\xC3\xA4m.anim.toml", n));
    CHECK_FALSE(clip_name_from_file(std::string(65, 'a') + ".anim.toml", n));
    CHECK_FALSE(clip_name_from_file("", n));
    CHECK(n == "untouched");
}

// Spec "Clip files": A clip is found.
TEST_CASE("anim registry: a clip is found under <mod id>:<file stem>") {
    std::vector<BuildLine> lines;
    auto set = build_clip_set({src("demo", "mods/demo/assets/animations/slam.anim.toml", clip_text(400))}, nullptr, &lines);
    REQUIRE(set);
    REQUIRE(set->clips.size() == 1);
    const ClipEntry* e = set->find("demo:slam");
    REQUIRE(e);
    CHECK(e->id == "demo:slam");
    CHECK(e->mod_id == "demo");
    CHECK(e->file == "mods/demo/assets/animations/slam.anim.toml");
    CHECK(e->text_hash.size() == 64);
    CHECK(e->clip.duration_ms == 400);
    CHECK(e->clip.tracks.size() == 1);
    CHECK(set->find("demo:other") == nullptr);
    CHECK(set->find("slam") == nullptr);

    REQUIRE(lines.size() == 1);
    CHECK(lines[0].kind == BuildLine::Loaded);
    CHECK(lines[0].id == "demo:slam");
    CHECK(lines[0].text == "loaded the clip 'demo:slam' from mods/demo/assets/animations/slam.anim.toml");
}

TEST_CASE("anim registry: two mods may use one name, and no line list is needed") {
    auto set = build_clip_set({src("a_mod", "a/slam.anim.toml", clip_text(100)), src("b_mod", "b/slam.anim.toml", clip_text(200))},
                              nullptr, nullptr);
    REQUIRE(set->clips.size() == 2);
    CHECK(set->find("a_mod:slam")->clip.duration_ms == 100);
    CHECK(set->find("b_mod:slam")->clip.duration_ms == 200);
}

// Spec "Clip files": A duplicate id.
TEST_CASE("anim registry: a duplicate id is rejected, names both files, and the first stays") {
    std::vector<BuildLine> lines;
    auto set = build_clip_set({src("demo", "animations/Slam.anim.toml", clip_text(100)),
                               src("demo", "animations/slam.anim.toml", clip_text(200)),
                               src("demo", "animations/other.anim.toml", clip_text(300))},
                              nullptr, &lines);
    REQUIRE(set->clips.size() == 2);
    CHECK(set->find("demo:slam")->clip.duration_ms == 100);
    CHECK(set->find("demo:slam")->file == "animations/Slam.anim.toml");
    CHECK(set->find("demo:other")->clip.duration_ms == 300);

    REQUIRE(count(lines, BuildLine::Error) == 1);
    const BuildLine* e = first(lines, BuildLine::Error);
    CHECK(e->id == "demo:slam");
    CHECK(e->file == "animations/slam.anim.toml");
    CHECK(e->text == "animations/slam.anim.toml: not loaded: the clip id 'demo:slam' is already taken by animations/Slam.anim.toml");
    CHECK(count(lines, BuildLine::Loaded) == 2);
}

TEST_CASE("anim registry: a bad first file still owns its id") {
    std::vector<BuildLine> lines;
    auto set = build_clip_set({src("demo", "Slam.anim.toml", kBadText), src("demo", "slam.anim.toml", clip_text(200))}, nullptr, &lines);
    CHECK(set->clips.empty());
    bool named = false;
    for (auto& l : lines) named = named || l.text.find("already taken by Slam.anim.toml") != std::string::npos;
    CHECK(named);
}

// Task 3.3: "a bad clip rejected without touching the others".
// covers: ability-animation/Clip validation: A bad key (task 4.4 reads it as the log line a bad clip leaves)
TEST_CASE("anim registry: a bad clip beside a good one") {
    std::vector<BuildLine> lines;
    auto set = build_clip_set({src("demo", "bad.anim.toml", kBadText), src("demo", "good.anim.toml", clip_text(250)),
                               src("demo", "bad name.anim.toml", clip_text(250))},
                              nullptr, &lines);
    REQUIRE(set->clips.size() == 1);
    CHECK(set->find("demo:good")->clip.duration_ms == 250);
    CHECK(set->find("demo:bad") == nullptr);

    REQUIRE(count(lines, BuildLine::Error) == 2);
    const BuildLine* e = first(lines, BuildLine::Error);
    CHECK(e->id == "demo:bad");
    CHECK(e->text.rfind("bad.anim.toml:5: track[0].keys[0].t: ", 0) == 0);   // the file, the line and the field path
    CHECK(lines.back().kind == BuildLine::Error);
    CHECK(lines.back().id.empty());
    CHECK(lines.back().text.rfind("bad name.anim.toml: not loaded: the name before '.anim.toml'", 0) == 0);
    CHECK(count(lines, BuildLine::Kept) == 0);   // nothing to keep on a first load
}

// Spec "Hot reload": Edit while playing; A bad edit.
TEST_CASE("anim registry: a reload gives a new set and leaves the old one as it was") {
    const std::string slam = "mods/demo/assets/animations/slam.anim.toml", jab = "mods/demo/assets/animations/jab.anim.toml",
                      gone = "mods/demo/assets/animations/gone.anim.toml", fresh = "mods/demo/assets/animations/fresh.anim.toml";
    auto v1 = build_clip_set({src("demo", gone, clip_text(50)), src("demo", jab, clip_text(100)), src("demo", slam, clip_text(400))},
                             nullptr, nullptr);
    REQUIRE(v1->clips.size() == 3);
    const ClipEntry* slam1 = v1->find("demo:slam");
    const ClipEntry* jab1 = v1->find("demo:jab");

    // slam is edited, jab is not, gone is deleted, fresh is new.
    std::vector<BuildLine> lines;
    auto v2 = build_clip_set({src("demo", fresh, clip_text(70)), src("demo", jab, clip_text(100)), src("demo", slam, clip_text(500))},
                             v1.get(), &lines);
    REQUIRE(v2->clips.size() == 3);
    CHECK(v2->find("demo:slam")->clip.duration_ms == 500);
    CHECK(v2->find("demo:slam") != slam1);
    CHECK(v2->find("demo:jab") == jab1);   // the same entry, not parsed again
    CHECK(v2->find("demo:gone") == nullptr);
    CHECK(v2->find("demo:fresh")->clip.duration_ms == 70);
    CHECK(v2->hash != v1->hash);

    REQUIRE(lines.size() == 3);   // no line for jab
    CHECK(lines[0].kind == BuildLine::Loaded);
    CHECK(lines[0].id == "demo:fresh");
    CHECK(lines[1].kind == BuildLine::Reloaded);
    CHECK(lines[1].text == "reloaded the clip 'demo:slam' from " + slam);   // the log line names the file
    CHECK(lines[2].kind == BuildLine::Removed);
    CHECK(lines[2].text == "removed the clip 'demo:gone': " + gone + " is gone");

    // The old set is immutable: a reader that still holds it sees the old clip.
    CHECK(v1->clips.size() == 3);
    CHECK(v1->find("demo:slam") == slam1);
    CHECK(slam1->clip.duration_ms == 400);
    CHECK(v1->find("demo:gone")->clip.duration_ms == 50);

    // A bad edit: the clip stays as it was, the log names the line, and the hash does not move.
    lines.clear();
    auto v3 = build_clip_set({src("demo", fresh, clip_text(70)), src("demo", jab, clip_text(100)), src("demo", slam, kBadText)},
                             v2.get(), &lines);
    CHECK(v3->find("demo:slam") == v2->find("demo:slam"));
    CHECK(v3->find("demo:slam")->clip.duration_ms == 500);
    CHECK(v3->hash == v2->hash);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].kind == BuildLine::Error);
    CHECK(lines[0].text.rfind(slam + ":5: track[0].keys[0].t: ", 0) == 0);
    CHECK(lines[1].kind == BuildLine::Kept);
    CHECK(lines[1].id == "demo:slam");

    // The file is fixed: the clip follows it again.
    lines.clear();
    auto v4 = build_clip_set({src("demo", fresh, clip_text(70)), src("demo", jab, clip_text(100)), src("demo", slam, clip_text(600))},
                             v3.get(), &lines);
    CHECK(v4->find("demo:slam")->clip.duration_ms == 600);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].kind == BuildLine::Reloaded);

    // Nothing changed: the same entries, the same hash, no line.
    lines.clear();
    auto v5 = build_clip_set({src("demo", fresh, clip_text(70)), src("demo", jab, clip_text(100)), src("demo", slam, clip_text(600))},
                             v4.get(), &lines);
    CHECK(lines.empty());
    CHECK(v5->hash == v4->hash);
    CHECK(v5->find("demo:slam") == v4->find("demo:slam"));
}

TEST_CASE("anim registry: a file that cannot be read keeps the clip") {
    auto v1 = build_clip_set({src("demo", "slam.anim.toml", clip_text(400))}, nullptr, nullptr);
    std::vector<BuildLine> lines;
    auto v2 = build_clip_set({{"demo", "slam.anim.toml", "", true}, {"demo", "new.anim.toml", "", true}}, v1.get(), &lines);
    REQUIRE(v2->clips.size() == 1);
    CHECK(v2->find("demo:slam") == v1->find("demo:slam"));
    REQUIRE(lines.size() == 3);
    CHECK(lines[0].text == "slam.anim.toml: not loaded: the file could not be read");
    CHECK(lines[1].kind == BuildLine::Kept);
    CHECK(lines[2].kind == BuildLine::Error);
    CHECK(count(lines, BuildLine::Removed) == 0);
}

// Spec "Clips are data": the hash of the clip set.
TEST_CASE("anim registry: the hash of the set") {
    ClipSet none;
    // SHA-256 of nothing.
    CHECK(clip_set_hash(none) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(build_clip_set({}, nullptr, nullptr)->hash == clip_set_hash(none));

    ClipSource a = src("demo", "x/a.anim.toml", clip_text(100)), b = src("demo", "x/b.anim.toml", clip_text(200));
    auto ab = build_clip_set({a, b}, nullptr, nullptr);
    auto ba = build_clip_set({b, a}, nullptr, nullptr);
    CHECK(ab->hash.size() == 64);
    CHECK(ab->hash == ba->hash);                 // the order of the files does not count
    CHECK(ab->hash == clip_set_hash(*ab));
    CHECK(ab->hash != clip_set_hash(none));

    // The folder does not count; the text, the name and the mod do.
    ClipSource a_moved = src("demo", "y/a.anim.toml", clip_text(100));
    CHECK(build_clip_set({a_moved, b}, nullptr, nullptr)->hash == ab->hash);
    CHECK(build_clip_set({src("demo", "x/a.anim.toml", clip_text(101)), b}, nullptr, nullptr)->hash != ab->hash);
    CHECK(build_clip_set({src("demo", "x/c.anim.toml", clip_text(100)), b}, nullptr, nullptr)->hash != ab->hash);
    CHECK(build_clip_set({src("other", "x/a.anim.toml", clip_text(100)), src("other", "x/b.anim.toml", clip_text(200))}, nullptr,
                         nullptr)->hash != ab->hash);
    CHECK(build_clip_set({a}, nullptr, nullptr)->hash != ab->hash);
    // A rejected clip is not in the hash.
    CHECK(build_clip_set({a, b, src("demo", "x/bad.anim.toml", kBadText)}, nullptr, nullptr)->hash == ab->hash);
}

// Spec "Hot reload": one immutable snapshot, published with a single pointer swap. A replaced set lives while a Hold is alive.
TEST_CASE("anim registry: publish swaps one pointer, and a replaced set lives while a hold is alive") {
    ClipRegistry reg;
    const ClipSet* empty = reg.current_locked();
    REQUIRE(empty);
    CHECK(empty->clips.empty());
    CHECK(empty->hash == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    auto v1 = build_clip_set({src("demo", "slam.anim.toml", clip_text(400))}, reg.current_locked(), nullptr);
    const ClipSet* p1 = v1.get();
    reg.publish(std::move(v1));
    CHECK(reg.current_locked() == p1);
    CHECK(reg.alive() == 1);   // nothing held the empty set, so the publish freed it
    const ClipEntry* held = nullptr;
    {
        auto h = reg.hold();   // what a draw in progress holds
        CHECK(&h.set() == p1);
        held = h.set().find("demo:slam");

        auto v2 = build_clip_set({src("demo", "slam.anim.toml", clip_text(900))}, reg.current_locked(), nullptr);
        const ClipSet* p2 = v2.get();
        reg.publish(std::move(v2));
        CHECK(reg.current_locked() == p2);
        CHECK(reg.current_locked()->find("demo:slam")->clip.duration_ms == 900);
        CHECK(reg.alive() == 2);                   // v1 is held: the publish could not free it
        CHECK(held->clip.duration_ms == 400);      // still alive and unchanged
        CHECK(p1->find("demo:slam") == held);
    }
    reg.reclaim();                                 // the hold ended: v1 is freed now
    CHECK(reg.alive() == 1);
    CHECK(reg.current_locked()->find("demo:slam")->clip.duration_ms == 900);

    reg.publish(nullptr);                          // ignored
    CHECK(reg.alive() == 1);
}

TEST_CASE("anim registry: a reader on another thread sees whole sets only, and the replaced sets are freed once it stops") {
    ClipRegistry reg;
    std::atomic<bool> stop{false};
    std::atomic<int> bad{0};
    std::atomic<long> reads{0};
    std::thread reader([&] {
        while (!stop.load()) {
            auto h = reg.hold();
            const ClipSet& s = h.set();
            // Every published set has `n` clips, all with the duration `n`.
            for (auto& [id, e] : s.clips)
                if (e->clip.duration_ms != static_cast<float>(s.clips.size())) ++bad;
            ++reads;
        }
    });
    for (int n = 1; n <= 40; ++n) {
        std::vector<ClipSource> sources;
        for (int i = 0; i < n; ++i) sources.push_back(src("demo", "c" + std::to_string(i) + ".anim.toml", clip_text(n)));
        reg.publish(build_clip_set(sources, reg.current_locked(), nullptr));
    }
    while (reads.load() < 100) std::this_thread::yield();
    stop = true;
    reader.join();
    CHECK(bad.load() == 0);
    CHECK(reg.current_locked()->clips.size() == 40);
    reg.reclaim();                                 // no hold is left: only the current set remains
    CHECK(reg.alive() == 1);
}

TEST_CASE("anim registry: clip_set_with adds or replaces one clip and leaves the rest and the old set alone") {
    // covers: game-api/Define from text: Publish an in-memory clip
    auto base = build_clip_set({src("demo", "a.anim.toml", clip_text(300)), src("demo", "b.anim.toml", clip_text(400))}, nullptr, nullptr);
    REQUIRE(base->clips.size() == 2);
    const std::string base_hash = base->hash;

    std::vector<BuildLine> lines;
    auto added = clip_set_with(*base, src("demo", "(editor)/c.anim.toml", clip_text(500)), &lines);
    REQUIRE(added);
    CHECK(added->clips.size() == 3);
    REQUIRE(added->find("demo:c"));
    CHECK(added->find("demo:c")->clip.duration_ms == 500);
    CHECK(added->find("demo:a") == base->find("demo:a"));   // the same entry, shared
    CHECK(added->hash != base_hash);
    CHECK(added->hash == clip_set_hash(*added));
    CHECK(count(lines, BuildLine::Loaded) == 1);

    auto replaced = clip_set_with(*added, src("demo", "(editor)/a.anim.toml", clip_text(900)), nullptr);
    REQUIRE(replaced);
    CHECK(replaced->clips.size() == 3);
    CHECK(replaced->find("demo:a")->clip.duration_ms == 900);
    CHECK(replaced->find("demo:b") == base->find("demo:b"));

    // the sets it came from are as they were
    CHECK(base->clips.size() == 2);
    CHECK(base->hash == base_hash);
    CHECK(base->find("demo:a")->clip.duration_ms == 300);
    CHECK(added->find("demo:a")->clip.duration_ms == 300);
}

TEST_CASE("anim registry: clip_set_with rejects a bad clip with its errors and no new set") {
    // covers: game-api/Define from text: A bad text
    auto base = build_clip_set({src("demo", "a.anim.toml", clip_text(300))}, nullptr, nullptr);
    std::vector<BuildLine> lines;
    CHECK_FALSE(clip_set_with(*base, src("demo", "(editor)/bad.anim.toml", kBadText), &lines));
    REQUIRE(count(lines, BuildLine::Error) >= 1);
    const std::string& text = first(lines, BuildLine::Error)->text;
    CHECK(text.find("(editor)/bad.anim.toml:") == 0);
    CHECK(text.find("keys[0].t") != std::string::npos);
    CHECK(base->clips.size() == 1);

    // a bad clip does not remove a good clip of the same id from the set it came from
    auto same = clip_set_with(*base, src("demo", "(editor)/a.anim.toml", kBadText), nullptr);
    CHECK_FALSE(same);
    CHECK(base->find("demo:a")->clip.duration_ms == 300);
}
