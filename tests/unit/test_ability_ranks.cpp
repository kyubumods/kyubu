#include "doctest.h"

#include <filesystem>

#include "ability_ranks.h"
#include "common.h"

using namespace kyubu;

namespace {

struct TempDir {
    std::filesystem::path dir;
    TempDir(const char* name) {
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        dir = std::filesystem::path(tmp) / (std::string("kyubu-ranks-") + name + "-" + std::to_string(GetCurrentProcessId()));
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    std::string file() const { return (dir / "Save" / "characters.kyubu-abilities.json").string(); }
    std::string text(const std::string& path) const {
        auto d = read_file(path);
        return d ? std::string(d->begin(), d->end()) : std::string("<missing>");
    }
};

const char* kBurst = "fourth_ability_demo:burst";

}  // namespace

TEST_CASE("ability ranks: rank 1 by default, no file, no character") {
    TempDir t("default");
    AbilityRanks r;
    CHECK(r.load(t.file()));                       // a missing file is an empty store
    CHECK(r.get(kBurst, 5) == 1);                  // nobody selected
    CHECK(r.set(kBurst, 3, 5) == -1);              // nobody selected: nothing is written
    CHECK(!file_exists(t.file()));
    r.select(0, "Tester");
    CHECK(r.get(kBurst, 5) == 1);
    CHECK(!file_exists(t.file()));                 // selecting writes nothing
}

TEST_CASE("ability ranks: set clamps, writes the file, and a new session reads it back") {
    TempDir t("roundtrip");
    {
        AbilityRanks r;
        r.load(t.file());
        r.select(0, "Tester");
        CHECK(r.set(kBurst, 3, 5) == 3);
        CHECK(r.set("a:b", 9, 5) == 5);            // above the maximum
        CHECK(r.set("a:c", 0, 5) == 0);            // 0 is a rank: not learned
        CHECK(!r.dirty());
        CHECK(!file_exists(t.file() + ".tmp"));    // the temporary file is gone after the rename
    }
    AbilityRanks r;
    CHECK(r.load(t.file()));
    r.select(0, "Tester");
    CHECK(r.get(kBurst, 5) == 3);
    CHECK(r.get("a:b", 5) == 5);
    CHECK(r.get("a:c", 5) == 0);
    CHECK(r.get("a:never", 5) == 1);
    CHECK(r.get(kBurst, 2) == 2);                  // the maximum came down since: clamped on read...
    CHECK(r.get(kBurst, 5) == 3);                  // ...and the stored value is not lost
    auto j = nlohmann::json::parse(t.text(t.file()));
    CHECK(j["format"] == 1);
    CHECK(j["characters"]["0"]["name"] == "Tester");
    CHECK(j["characters"]["0"]["ranks"][kBurst] == 3);
}

TEST_CASE("ability ranks: another character, and the same slot with another name") {
    TempDir t("characters");
    AbilityRanks r;
    r.load(t.file());
    r.select(0, "Tester");
    r.set(kBurst, 4, 5);
    r.select(1, "Other");
    CHECK(r.get(kBurst, 5) == 1);                  // another character
    r.set("a:b", 2, 5);
    r.select(0, "Tester");
    CHECK(r.get(kBurst, 5) == 4);                  // the first one is untouched
    r.select(0, "NewGuy");                         // the slot was reused
    CHECK(r.get(kBurst, 5) == 1);                  // the file is never applied to another name
    CHECK(t.text(t.file()).find("Tester") != std::string::npos);   // reading does not change it
    r.set(kBurst, 2, 5);                           // the first change takes the slot over
    r.select(0, "Tester");
    CHECK(r.get(kBurst, 5) == 1);
    r.select(1, "Other");
    CHECK(r.get("a:b", 5) == 2);
}

TEST_CASE("ability ranks: the ranks of a missing mod are kept") {
    TempDir t("missing");
    make_dirs(dir_of(t.file()));
    write_text(t.file(), R"({"format":1,"characters":{"0":{"name":"Tester","extra":true,"ranks":{"gone:thing":4,"fourth_ability_demo:burst":2}},"7":{"name":"Far"}}})");
    AbilityRanks r;
    CHECK(r.load(t.file()));
    r.select(0, "Tester");
    CHECK(r.get(kBurst, 5) == 2);
    CHECK(r.set(kBurst, 3, 5) == 3);               // the mod "gone" is not loaded: not asked about
    auto j = nlohmann::json::parse(t.text(t.file()));
    CHECK(j["characters"]["0"]["ranks"]["gone:thing"] == 4);
    CHECK(j["characters"]["0"]["ranks"][kBurst] == 3);
    CHECK(j["characters"]["0"]["extra"] == true);   // keys this version does not know stay
    CHECK(j["characters"]["7"]["name"] == "Far");
    r.select(0, "Tester");
    r.load(t.file());                              // "the mod returns" in a later session
    r.select(0, "Tester");
    CHECK(r.get("gone:thing", 5) == 4);
}

TEST_CASE("ability ranks: odd values read as the default") {
    TempDir t("odd");
    make_dirs(dir_of(t.file()));
    write_text(t.file(), R"({"format":1,"characters":{"0":{"name":"T","ranks":{"a:neg":-3,"a:str":"x","a:flt":2.5,"a:huge":99999999999,"a:ok":2}},"1":5,"2":{"name":"U","ranks":[1]}}})");
    AbilityRanks r;
    CHECK(r.load(t.file()));
    r.select(0, "T");
    CHECK(r.get("a:neg", 5) == 1);
    CHECK(r.get("a:str", 5) == 1);
    CHECK(r.get("a:flt", 5) == 1);
    CHECK(r.get("a:huge", 5) == 5);
    CHECK(r.get("a:ok", 5) == 2);
    r.select(1, "x");
    CHECK(r.get("a:ok", 5) == 1);                  // an entry that is not an object
    CHECK(r.set("a:ok", 3, 5) == 3);               // is replaced by a good one
    r.select(2, "U");
    CHECK(r.get("a:ok", 5) == 1);                  // "ranks" that is not an object
    CHECK(r.set("a:ok", 4, 5) == 4);
    r.select(2, "U");
    CHECK(r.get("a:ok", 5) == 4);
}

TEST_CASE("ability ranks: an unreadable file is logged, read as empty, and kept as .bad") {
    const char* bad[] = {"", "{not json", "[1,2]", R"({"characters":{}})", R"({"format":2,"characters":{}})",
                         R"({"format":1})", R"({"format":1,"characters":[]})"};
    int n = 0;
    for (const char* content : bad) {
        TempDir t(("corrupt" + std::to_string(n++)).c_str());
        make_dirs(dir_of(t.file()));
        write_text(t.file(), content);
        AbilityRanks r;
        CHECK_FALSE(r.load(t.file()));
        CHECK(r.unreadable());
        r.select(0, "Tester");
        CHECK(r.get(kBurst, 5) == 1);
        CHECK(t.text(t.file()) == content);         // reading never changes it
        CHECK(r.set(kBurst, 3, 5) == 3);            // the game goes on
        CHECK(t.text(t.file() + ".bad") == content);
        CHECK(!r.unreadable());
        AbilityRanks again;
        CHECK(again.load(t.file()));
        again.select(0, "Tester");
        CHECK(again.get(kBurst, 5) == 3);
    }
}

TEST_CASE("ability ranks: a failed write keeps the rank in memory and the old file") {
    TempDir t("nowrite");
    AbilityRanks r;
    r.load(t.file());
    r.select(0, "Tester");
    CHECK(r.set(kBurst, 2, 5) == 2);
    std::string before = t.text(t.file());
    // A directory where the temporary file belongs: the write fails.
    std::filesystem::create_directories(t.file() + ".tmp");
    CHECK(r.set(kBurst, 4, 5) == -1);
    CHECK(r.get(kBurst, 5) == 4);
    CHECK(r.dirty());
    CHECK(t.text(t.file()) == before);
    std::filesystem::remove_all(t.file() + ".tmp");
    CHECK(r.save());                               // the next save catches up
    CHECK_FALSE(r.dirty());
    AbilityRanks again;
    again.load(t.file());
    again.select(0, "Tester");
    CHECK(again.get(kBurst, 5) == 4);
}
