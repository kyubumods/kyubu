// Task 7.3: the checks of a downloaded zip before it is staged (design D6, spec "Download integrity"). The zips are built in memory
// with miniz's writer; the fixture catalog's own zips are checked from disk.
// Build: -I mods-gameplay/mod_manager/src -I third_party/miniz -I third_party/doctest, with third_party/miniz/miniz.c compiled as C.
// Run from the repository root (it opens tests/fixtures/catalog/mods/...).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "zipcheck.h"

#include <cstring>
#include <utility>

namespace {

const char* kManifest = "# a test\nid = \"cat_alpha\"\nname = \"Alpha\"\nversion = \"1.1.0\"\n[dependencies]\nid = \"other\"\n";

// A zip of these (name, text) entries.
std::string make_zip(const std::vector<std::pair<std::string, std::string>>& files) {
    mz_zip_archive z = {};
    REQUIRE(mz_zip_writer_init_heap(&z, 0, 0));
    for (const auto& [name, text] : files)
        REQUIRE(mz_zip_writer_add_mem(&z, name.c_str(), text.data(), text.size(), MZ_DEFAULT_COMPRESSION));
    void* buf = nullptr;
    size_t size = 0;
    REQUIRE(mz_zip_writer_finalize_heap_archive(&z, &buf, &size));
    std::string out(static_cast<const char*>(buf), size);
    mz_free(buf);
    mz_zip_writer_end(&z);
    return out;
}

zipcheck::Result check(const std::string& zip, const char* id = "cat_alpha", const char* version = "1.1.0") {
    return zipcheck::check_memory(zip.data(), zip.size(), id, version);
}

}  // namespace

TEST_CASE("mod.toml at the root") {
    const zipcheck::Result r = check(make_zip({{"mod.toml", kManifest}, {"scripts/main.lua", "x"}, {"config.toml", ""}}));
    CHECK(r.ok);
    CHECK(r.reason.empty());
}

TEST_CASE("mod.toml in one top-level folder") {
    CHECK(check(make_zip({{"cat_alpha/", ""}, {"cat_alpha/mod.toml", kManifest}, {"cat_alpha/scripts/main.lua", "x"}})).ok);
}

TEST_CASE("two top-level folders: no mod.toml to trust") {
    const zipcheck::Result r = check(make_zip({{"a/mod.toml", kManifest}, {"b/readme.txt", "x"}}));
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "The zip has no mod.toml at its root or in one top-level folder.");
}

TEST_CASE("mod.toml two folders deep is not found") {
    CHECK_FALSE(check(make_zip({{"a/b/mod.toml", kManifest}})).ok);
}

TEST_CASE("an entry that leaves the folder is rejected") {
    for (const char* bad : {"../zlib1.dll", "a/../../x.dll", "/abs.dll", "\\abs.dll", "C:/Windows/x.dll", "a\\..\\..\\x.dll", "./mod.toml", "a//b"}) {
        CAPTURE(bad);
        // miniz's writer refuses such names, so the zip is built with a stand-in of the same length, then the name is patched in
        // (local header and central directory both).
        const std::string stand_in(std::strlen(bad), 'q');
        std::string zip = make_zip({{"mod.toml", kManifest}, {stand_in, "evil"}});
        for (size_t at = zip.find(stand_in); at != std::string::npos; at = zip.find(stand_in, at + 1)) zip.replace(at, stand_in.size(), bad);
        const zipcheck::Result r = check(zip);
        CHECK_FALSE(r.ok);
        CHECK(r.reason.find("an entry outside its folder") != std::string::npos);
    }
    CHECK(zipcheck::safe_name("a..b/c.lua"));   // dots inside a name are fine
    CHECK(zipcheck::safe_name("dir/"));
}

TEST_CASE("the id and version must be the promised ones") {
    const std::string z = make_zip({{"mod.toml", kManifest}});
    zipcheck::Result r = check(z, "cat_beta", "1.1.0");
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "The zip holds 'cat_alpha', not 'cat_beta'.");
    r = check(z, "cat_alpha", "1.0.0");
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "The zip holds version 1.1.0, not 1.0.0.");
}

TEST_CASE("not a zip, an empty zip, no manifest") {
    const std::string junk = "this is not a zip file at all";
    CHECK(zipcheck::check_memory(junk.data(), junk.size(), "a", "1.0.0").reason == "The download is not a readable zip.");
    CHECK_FALSE(check(make_zip({})).ok);
    CHECK_FALSE(check(make_zip({{"scripts/main.lua", "x"}})).ok);
}

TEST_CASE("an unpacked size of 512 MB or more is rejected") {
    // The size is read from the zip's directory; the data of the entry is never unpacked, so a declared size is enough.
    mz_zip_archive z = {};
    REQUIRE(mz_zip_writer_init_heap(&z, 0, 0));
    REQUIRE(mz_zip_writer_add_mem(&z, "mod.toml", kManifest, std::strlen(kManifest), MZ_DEFAULT_COMPRESSION));
    const char fake[4] = {3, 0, 0, 0};
    REQUIRE(mz_zip_writer_add_mem_ex(&z, "big.bin", fake, sizeof fake, nullptr, 0, MZ_ZIP_FLAG_COMPRESSED_DATA, 600ull << 20, 0));
    void* buf = nullptr;
    size_t size = 0;
    REQUIRE(mz_zip_writer_finalize_heap_archive(&z, &buf, &size));
    const std::string zip(static_cast<const char*>(buf), size);
    mz_free(buf);
    mz_zip_writer_end(&z);
    const zipcheck::Result r = check(zip);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "The zip unpacks to more than 512 MB.");
}

TEST_CASE("top-level strings of mod.toml") {
    CHECK(zipcheck::top_string(kManifest, "id") == "cat_alpha");
    CHECK(zipcheck::top_string(kManifest, "version") == "1.1.0");
    CHECK(zipcheck::top_string("idx = \"no\"\nid='yes'\n", "id") == "yes");
    CHECK(zipcheck::top_string("[t]\nid = \"in a table\"\n", "id").empty());
    CHECK(zipcheck::top_string("id = 3\n", "id").empty());
    CHECK(zipcheck::top_string("id = \"crlf\"\r\n", "id") == "crlf");
}

TEST_CASE("the fixture catalog's zips pass from disk") {
    CHECK(zipcheck::check_file("tests/fixtures/catalog/mods/cat_alpha/cat_alpha-1.1.0.zip", "cat_alpha", "1.1.0").ok);
    CHECK(zipcheck::check_file("tests/fixtures/catalog/mods/cat_lib/cat_lib-1.0.0.zip", "cat_lib", "1.0.0").ok);
    CHECK_FALSE(zipcheck::check_file("tests/fixtures/catalog/mods/cat_alpha/cat_alpha-1.0.0.zip", "cat_alpha", "1.1.0").ok);
    CHECK(zipcheck::check_file("tests/fixtures/catalog/no-such.zip", "a", "1.0.0").reason == "The download is not a readable zip.");
}
