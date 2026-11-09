// Task 7.1: the index parser, the signature check and the choice between the fetched and the saved copy.
// Build with -DKYUBU_MM_TEST_KEY -DMM_FIXTURE_DIR="<repo>/tests/fixtures/catalog" and link third_party/monocypher/*.o.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <cstdio>

#include "index.h"

namespace {
std::string slurp(const std::string& name) {
    std::string out;
    if (FILE* f = std::fopen((std::string(MM_FIXTURE_DIR) + "/" + name).c_str(), "rb")) {
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
        std::fclose(f);
    }
    return out;
}
const std::vector<keys::Key> kKeys = keys::trusted();
}  // namespace

TEST_CASE("the fixture catalog parses") {
    const std::string text = slurp("index.json");
    REQUIRE(!text.empty());
    const cindex::Parsed p = cindex::parse(text);
    REQUIRE(p.ok);
    CHECK(p.index.name == "Kyubu mods");
    REQUIRE(p.index.mods.size() == 3);
    CHECK(p.index.mods[0].id == "cat_alpha");
    REQUIRE(p.index.mods[0].versions.size() == 2);
    CHECK(p.index.mods[0].versions[1].version == "1.1.0");
    CHECK(p.index.mods[0].versions[0].needs.at("cat_lib") == "^1.0");
    CHECK(p.index.mods[0].versions[0].url == "mods/cat_alpha/cat_alpha-1.0.0.zip");
    REQUIRE(p.index.packs.size() == 1);
    CHECK(p.index.packs[0].mods.at("cat_alpha") == "1.0.0");
    CHECK(p.index.packs[0].reset_unlisted);
    CHECK(p.index.packs[0].config.find("strength") != std::string::npos);
}

TEST_CASE("the parser rejects bad entries") {
    const std::string good = slurp("index.json");
    auto replace = [&](const std::string& a, const std::string& b) {
        std::string t = good;
        const size_t i = t.find(a);
        REQUIRE(i != std::string::npos);
        return t.replace(i, a.size(), b);
    };
    CHECK(!cindex::parse("").ok);
    CHECK(!cindex::parse("[]").ok);
    CHECK(!cindex::parse("{\"format\": 2}").ok);
    CHECK(!cindex::parse(replace("mods/cat_alpha/cat_alpha-1.0.0.zip", "../cat_alpha-1.0.0.zip")).ok);
    CHECK(!cindex::parse(replace("mods/cat_alpha/cat_alpha-1.0.0.zip", "https://evil.example/a.zip")).ok);
    CHECK(!cindex::parse(replace("mods/cat_alpha/cat_alpha-1.0.0.zip", "/abs.zip")).ok);
    CHECK(!cindex::parse(replace("5525a50d165a94a1", "ZZ25a50d165a94a1")).ok);
    CHECK(!cindex::parse(replace("\"id\": \"cat_beta\"", "\"id\": \"cat_alpha\"")).ok);
    CHECK(!cindex::parse(replace("\"size\": 547", "\"size\": 0")).ok);
    CHECK(!cindex::parse(replace("\"id\": \"cat_beta\"", "\"id\": \"Bad Id\"")).ok);
}

TEST_CASE("the signature holds only for the exact bytes and a trusted key") {
    REQUIRE(!kKeys.empty());
    const std::string text = slurp("index.json"), sig = slurp("index.json.sig");
    REQUIRE(sig.size() == 64);
    CHECK(cindex::verify(text, sig, kKeys));
    std::string tampered = text;
    tampered[tampered.size() / 2] ^= 1;
    CHECK(!cindex::verify(tampered, sig, kKeys));
    CHECK(!cindex::verify(text + " ", sig, kKeys));
    std::string badsig = sig;
    badsig[10] ^= 1;
    CHECK(!cindex::verify(text, badsig, kKeys));
    CHECK(!cindex::verify(text, sig.substr(0, 63), kKeys));
    keys::Key other = kKeys[0];
    other[0] ^= 1;
    CHECK(!cindex::verify(text, sig, {other}));
    CHECK(!cindex::verify(text, sig, {}));
    CHECK(cindex::verify(text, sig, {other, kKeys[0]}));
}

TEST_CASE("resolve: online, rejected, offline, nothing") {
    const std::string text = slurp("index.json"), sig = slurp("index.json.sig");
    cindex::Fetched net;
    net.reached = true;
    net.index = text;
    net.sig = sig;
    net.etag = "\"abc\"";

    cindex::Loaded a = cindex::resolve(net, "", "", "", kKeys);
    CHECK(a.state == cindex::State::Online);
    CHECK(a.save);
    CHECK(a.index_text == text);
    CHECK(a.etag == "\"abc\"");
    CHECK(a.index.mods.size() == 3);

    // A tampered fetch is rejected whole, offers nothing, and does not replace the saved copy.
    cindex::Fetched bad = net;
    bad.index[bad.index.size() / 2] ^= 1;
    cindex::Loaded b = cindex::resolve(bad, text, sig, "", kKeys);
    CHECK(b.state == cindex::State::Rejected);
    CHECK(!b.save);
    CHECK(b.index.mods.empty());
    CHECK(b.message == "The catalog could not be verified.");

    // A missing signature is the same.
    cindex::Fetched nosig = net;
    nosig.sig.clear();
    CHECK(cindex::resolve(nosig, text, sig, "", kKeys).state == cindex::State::Rejected);

    // Signed but not parseable (format 2): rejected.
    cindex::Fetched fmt = net;
    fmt.index = "{\"format\": 2}";
    CHECK(cindex::resolve(fmt, "", "", "", kKeys).state == cindex::State::Rejected);

    // Unreachable: the saved copy, checked again.
    cindex::Fetched off;
    cindex::Loaded c = cindex::resolve(off, text, sig, "\"abc\"", kKeys);
    CHECK(c.state == cindex::State::Offline);
    CHECK(!c.save);
    CHECK(c.index.mods.size() == 3);
    // A saved copy that no longer verifies (edited on disk) is not used.
    std::string edited = text;
    edited[edited.size() / 3] ^= 1;
    CHECK(cindex::resolve(off, edited, sig, "", kKeys).state == cindex::State::None);
    CHECK(cindex::resolve(off, "", "", "", kKeys).state == cindex::State::None);

    // 304: the saved copy is current and counts as online.
    cindex::Fetched nm;
    nm.reached = true;
    nm.not_modified = true;
    cindex::Loaded d = cindex::resolve(nm, text, sig, "\"abc\"", kKeys);
    CHECK(d.state == cindex::State::Online);
    CHECK(!d.save);
    CHECK(cindex::resolve(nm, edited, sig, "", kKeys).state == cindex::State::None);
}
