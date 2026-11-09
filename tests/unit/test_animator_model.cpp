// Unit tests of the Animator's editing model (openspec add-ability-animation task 6.1): clip names, the new clip text, the
// undo stack, the atomic save and the outside-edit check, on real files in a temp folder. No game.
#include "doctest.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "model.h"

using namespace kanimator;

namespace {

namespace fs = std::filesystem;

// A fresh folder for one test, removed when the test ends.
struct TempDir {
    fs::path dir;
    TempDir() {
        auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        dir = fs::temp_directory_path() / ("kyubu_animator_test_" + std::to_string(stamp));
        fs::create_directories(dir);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    std::string file(const char* name) const { return (dir / name).string(); }
};

std::string read_all(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void write_all(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

}  // namespace

TEST_CASE("clip names follow the define rule") {
    CHECK(valid_clip_name("burst"));
    CHECK(valid_clip_name("a_b-c.d9"));
    CHECK(valid_clip_name(std::string(64, 'a')));
    CHECK(!valid_clip_name(""));
    CHECK(!valid_clip_name("Burst"));
    CHECK(!valid_clip_name("two words"));
    CHECK(!valid_clip_name(std::string(65, 'a')));

    std::string name;
    CHECK(name_from_path("C:\\mods\\x\\assets\\animations\\burst.anim.toml", name));
    CHECK(name == "burst");
    CHECK(name_from_path("burst.anim.toml", name));
    CHECK(name == "burst");
    CHECK(!name_from_path("C:\\x\\burst.toml", name));
    CHECK(!name_from_path("C:\\x\\.anim.toml", name));
    CHECK(!name_from_path("C:\\x\\Burst.anim.toml", name));
}

TEST_CASE("the new clip text names the clip, uses a duration in ms and has a track") {
    std::string t = new_clip_text("burst");
    CHECK(t.find("# burst:") == 0);
    CHECK(t.find("duration = 1000\n") != std::string::npos);
    CHECK(t.find("[[track]]") != std::string::npos);
}

TEST_CASE("a new clip is dirty until saved, and the save leaves no temp file") {
    TempDir td;
    std::string path = td.file("burst.anim.toml");
    ClipDoc doc;
    doc.start_new(path, "first\n");
    CHECK(doc.dirty());
    CHECK(!fs::exists(path));

    std::string err;
    REQUIRE(doc.save(err));
    CHECK(!doc.dirty());
    CHECK(read_all(path) == "first\n");
    CHECK(!fs::exists(path + ".tmp"));

    doc.set_text("second\n");
    CHECK(doc.dirty());
    REQUIRE(doc.save(err));
    CHECK(read_all(path) == "second\n");
}

TEST_CASE("open reads the file and starts with no undo steps") {
    TempDir td;
    std::string path = td.file("a.anim.toml");
    write_all(path, "on disk\n");
    ClipDoc doc;
    std::string err;
    REQUIRE(doc.open(path, err));
    CHECK(doc.text() == "on disk\n");
    CHECK(!doc.dirty());
    CHECK(doc.undo_depth() == 0);
}

TEST_CASE("open of a missing file fails and leaves the document as it was") {
    TempDir td;
    ClipDoc doc;
    doc.start_new(td.file("keep.anim.toml"), "kept");
    std::string err;
    CHECK(!doc.open(td.file("missing.anim.toml"), err));
    CHECK(!err.empty());
    CHECK(doc.path() == td.file("keep.anim.toml"));
    CHECK(doc.text() == "kept");
}

TEST_CASE("undo steps back, the same text is no step, and the stack keeps 100 steps") {
    ClipDoc doc;
    doc.start_new("", "v0");
    doc.set_text("v0");
    CHECK(doc.undo_depth() == 0);

    for (int i = 1; i <= 105; ++i) doc.set_text("v" + std::to_string(i));
    CHECK(doc.undo_depth() == kUndoDepth);
    for (size_t i = 0; i < kUndoDepth; ++i) REQUIRE(doc.undo());
    CHECK(doc.text() == "v5");
    CHECK(!doc.undo());
}

TEST_CASE("dirty means the text differs from the file, so undo back to it clears dirty") {
    TempDir td;
    ClipDoc doc;
    doc.start_new(td.file("a.anim.toml"), "x");
    std::string err;
    REQUIRE(doc.save(err));
    doc.set_text("y");
    CHECK(doc.dirty());
    CHECK(doc.undo());
    CHECK(!doc.dirty());
}

TEST_CASE("an outside edit is seen, and reload takes the file and clears the undo stack") {
    TempDir td;
    std::string path = td.file("a.anim.toml");
    ClipDoc doc;
    doc.start_new(path, "one\n");
    std::string err;
    REQUIRE(doc.save(err));
    CHECK(!doc.disk_changed());

    doc.set_text("one two\n");
    REQUIRE(doc.save(err));
    CHECK(doc.undo_depth() == 1);

    write_all(path, "outside edit, longer than before\n");
    CHECK(doc.disk_changed());
    REQUIRE(doc.reload(err));
    CHECK(doc.text() == "outside edit, longer than before\n");
    CHECK(!doc.dirty());
    CHECK(doc.undo_depth() == 0);
    CHECK(!doc.disk_changed());
}

TEST_CASE("accept_disk keeps the text, takes the file as known, and a save then overwrites it") {
    TempDir td;
    std::string path = td.file("a.anim.toml");
    ClipDoc doc;
    doc.start_new(path, "mine\n");
    std::string err;
    REQUIRE(doc.save(err));
    doc.set_text("mine 2\n");

    write_all(path, "theirs, and longer\n");
    CHECK(doc.disk_changed());
    doc.accept_disk();
    CHECK(!doc.disk_changed());
    CHECK(doc.text() == "mine 2\n");
    CHECK(doc.dirty());

    REQUIRE(doc.save(err));
    CHECK(read_all(path) == "mine 2\n");
}

TEST_CASE("reload of a file with the same text keeps the undo stack") {
    TempDir td;
    std::string path = td.file("a.anim.toml");
    ClipDoc doc;
    doc.start_new(path, "one\n");
    std::string err;
    REQUIRE(doc.save(err));
    doc.set_text("two\n");
    REQUIRE(doc.save(err));
    REQUIRE(doc.undo_depth() == 1);

    write_all(path, "two\n");   // an outside save of the same text: the time changes, the text does not
    REQUIRE(doc.reload(err));
    CHECK(doc.text() == "two\n");
    CHECK(!doc.dirty());
    CHECK(doc.undo_depth() == 1);
    CHECK(!doc.disk_changed());
}

TEST_CASE("a temp file left by a crash does not change the clip, and a save never shows half a file") {
    TempDir td;
    std::string path = td.file("a.anim.toml");
    ClipDoc doc;
    doc.start_new(path, "old text\n");
    std::string err;
    REQUIRE(doc.save(err));

    // The save writes the temp file whole, then replaces the file: a half temp file beside the clip is not the clip.
    write_all(path + ".tmp", "half writ");
    CHECK(read_all(path) == "old text\n");
    CHECK(!doc.disk_changed());
    doc.set_text("new text, longer\n");
    REQUIRE(doc.save(err));
    CHECK(read_all(path) == "new text, longer\n");
    CHECK(!fs::exists(path + ".tmp"));
}

TEST_CASE("the autosave goes beside the file after the interval, never to the file itself") {
    TempDir td;
    std::string path = td.file("a.anim.toml");
    CHECK(autosave_path(path) == path + ".autosave");
    ClipDoc doc;
    doc.start_new(path, "saved\n");
    std::string err;
    REQUIRE(doc.save(err));

    CHECK(!doc.autosave_tick(1000, kAutosaveMs, err));   // nothing is unsaved
    doc.set_text("edited\n");
    CHECK(!doc.autosave_tick(2000, kAutosaveMs, err));   // the first look at the unsaved text starts the wait
    CHECK(!fs::exists(autosave_path(path)));
    CHECK(!doc.autosave_tick(2000 + kAutosaveMs - 1, kAutosaveMs, err));
    CHECK(doc.autosave_tick(2000 + kAutosaveMs, kAutosaveMs, err));
    CHECK(read_all(autosave_path(path)) == "edited\n");
    CHECK(read_all(path) == "saved\n");
    CHECK(!fs::exists(autosave_path(path) + ".tmp"));

    // Nothing new: nothing written. A new edit waits for the interval from the last autosave.
    CHECK(!doc.autosave_tick(2000 + 5 * kAutosaveMs, kAutosaveMs, err));
    doc.set_text("edited again\n");
    CHECK(!doc.autosave_tick(2000 + kAutosaveMs + 10, kAutosaveMs, err));
    CHECK(doc.autosave_tick(2000 + 2 * kAutosaveMs, kAutosaveMs, err));
    CHECK(read_all(autosave_path(path)) == "edited again\n");
}

TEST_CASE("a save, or an undo back to the file, removes the autosave") {
    TempDir td;
    std::string path = td.file("a.anim.toml");
    ClipDoc doc;
    doc.start_new(path, "saved\n");
    std::string err;
    REQUIRE(doc.save(err));

    doc.set_text("edited\n");
    doc.autosave_tick(0, kAutosaveMs, err);
    REQUIRE(doc.autosave_tick(kAutosaveMs, kAutosaveMs, err));
    REQUIRE(fs::exists(autosave_path(path)));
    REQUIRE(doc.save(err));
    CHECK(!fs::exists(autosave_path(path)));

    doc.set_text("edited 2\n");
    doc.autosave_tick(100000, kAutosaveMs, err);
    REQUIRE(doc.autosave_tick(100000 + kAutosaveMs, kAutosaveMs, err));
    REQUIRE(fs::exists(autosave_path(path)));
    REQUIRE(doc.undo());
    CHECK(!doc.dirty());
    CHECK(!doc.autosave_tick(100000 + 2 * kAutosaveMs, kAutosaveMs, err));
    CHECK(!fs::exists(autosave_path(path)));
}

TEST_CASE("an autosave left by an earlier run is found, and one that matches the file is not") {
    TempDir td;
    std::string path = td.file("a.anim.toml");
    write_all(path, "on disk\n");
    write_all(autosave_path(path), "from a crash\n");

    ClipDoc doc;
    std::string err, found;
    REQUIRE(doc.open(path, err));
    CHECK(doc.find_autosave(found));
    CHECK(found == "from a crash\n");
    CHECK(fs::exists(autosave_path(path)));   // looking does not remove it

    // A document that is clean does not remove an autosave it did not write.
    CHECK(!doc.autosave_tick(0, kAutosaveMs, err));
    CHECK(!doc.autosave_tick(10 * kAutosaveMs, kAutosaveMs, err));
    CHECK(fs::exists(autosave_path(path)));

    doc.drop_autosave();
    CHECK(!fs::exists(autosave_path(path)));

    write_all(autosave_path(path), "on disk\n");   // the same as the file: nothing to recover
    CHECK(!doc.find_autosave(found));
    CHECK(!fs::exists(autosave_path(path)));
}

TEST_CASE("a document with no path has no autosave") {
    ClipDoc doc;
    doc.start_new("", "x");
    std::string err;
    CHECK(!doc.autosave_tick(0, kAutosaveMs, err));
    CHECK(!doc.autosave_tick(10 * kAutosaveMs, kAutosaveMs, err));
    std::string found;
    CHECK(!doc.find_autosave(found));
}
