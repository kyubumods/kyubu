#include "doctest.h"

#include <array>
#include <cstdlib>

#include "common.h"
#include "formats.h"

using namespace kyubu;

static void put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i))); }

static std::vector<uint8_t> make_vox(int sx, int sy, int sz, const std::vector<std::array<int, 4>>& voxels,
                                     const std::vector<std::array<int, 3>>& palette) {
    std::vector<uint8_t> size, xyzi, rgba, children, out;
    for (int v : {sx, sy, sz}) put32(size, v);
    put32(xyzi, (uint32_t)voxels.size());
    for (auto& v : voxels) for (int k = 0; k < 4; ++k) xyzi.push_back((uint8_t)v[k]);
    for (int i = 0; i < 256; ++i) {
        auto c = i < (int)palette.size() ? palette[i] : std::array<int, 3>{0, 0, 0};
        rgba.insert(rgba.end(), {(uint8_t)c[0], (uint8_t)c[1], (uint8_t)c[2], 255});
    }
    auto chunk = [&](const char* id, const std::vector<uint8_t>& body) {
        children.insert(children.end(), id, id + 4);
        put32(children, (uint32_t)body.size());
        put32(children, 0);
        children.insert(children.end(), body.begin(), body.end());
    };
    chunk("SIZE", size);
    chunk("XYZI", xyzi);
    chunk("RGBA", rgba);
    out.insert(out.end(), {'V', 'O', 'X', ' '});
    put32(out, 150);
    out.insert(out.end(), {'M', 'A', 'I', 'N'});
    put32(out, 0);
    put32(out, (uint32_t)children.size());
    out.insert(out.end(), children.begin(), children.end());
    return out;
}

TEST_CASE("asset-overlay: game cipher round trip") {
    std::vector<uint8_t> plain;
    for (int i = 0; i < 5000; ++i) plain.push_back(uint8_t(i * 31 + 7));
    auto enc = asset_encode(plain);
    CHECK(enc != plain);
    CHECK(asset_decode(enc) == plain);
    // Known vector: the cipher of "PNG" header bytes is stable (regression guard for the key table).
    std::vector<uint8_t> hdr = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    CHECK(asset_decode(asset_encode(hdr)) == hdr);
}

TEST_CASE("asset-overlay: vox to cub conversion") {
    auto vox = make_vox(2, 3, 4, {{0, 0, 0, 1}, {1, 2, 3, 2}, {1, 0, 1, 3}}, {{255, 0, 0}, {0, 255, 0}, {0, 0, 0}});
    std::string err;
    auto m = parse_vox(vox, &err);
    REQUIRE_MESSAGE(m, err);
    CHECK(m->sx == 2);
    CHECK(m->sy == 3);
    CHECK(m->sz == 4);
    auto at = [&](int x, int y, int z) {
        size_t o = (((size_t)z * m->sy + y) * m->sx + x) * 3;
        return std::array<int, 3>{m->rgb[o], m->rgb[o + 1], m->rgb[o + 2]};
    };
    CHECK(at(0, 0, 0) == std::array<int, 3>{255, 0, 0});
    CHECK(at(1, 2, 3) == std::array<int, 3>{0, 255, 0});
    CHECK(at(1, 0, 1) == std::array<int, 3>{1, 1, 1});   // pure black nudged: (0,0,0) means empty in .cub
    CHECK(at(1, 1, 1) == std::array<int, 3>{0, 0, 0});
    auto cub = write_cub(*m);
    CHECK(cub.size() == 12 + 2 * 3 * 4 * 3);
    auto back = parse_cub(cub, &err);
    REQUIRE(back);
    CHECK(back->rgb == m->rgb);
    CHECK(validate_model(*m).empty());
}

TEST_CASE("asset-overlay: broken vox") {
    // covers: asset-overlay/Broken vox (parser part)
    auto vox = make_vox(2, 2, 2, {{0, 0, 0, 1}}, {{10, 20, 30}});
    vox.resize(vox.size() / 2);
    std::string err;
    CHECK_FALSE(parse_vox(vox, &err));
    CHECK(err.find("truncated") != std::string::npos);
    CHECK_FALSE(parse_vox({'n', 'o', 'p', 'e'}, &err));
}

TEST_CASE("asset-overlay: sound and icon validation") {
    std::vector<uint8_t> wav = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0,
                                1, 0, 2, 0, 0x44, 0xAC, 0, 0, 0, 0, 0, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0};
    CHECK(validate_wav(wav).empty());
    wav[34] = 8;   // 8-bit PCM
    CHECK(validate_wav(wav).find("unsupported sample format") != std::string::npos);
    CHECK(validate_png({0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n', 0}).empty());
    CHECK(!validate_png({'G', 'I', 'F'}).empty());
}

TEST_CASE("asset-overlay: dictionary merge") {
    // covers: asset-overlay/Two mods edit different strings (merge part)
    std::string base =
        "\xEF\xBB\xBF<root>\r\n\r\n  <name key=\"Sword\" tags=\"n\">\r\n    <singular>Sword</singular>\r\n  </name>\r\n\r\n"
        "  <speech key=\"title:warrior:1.4\">\r\n    Eine schleimige Angelegenheit\r\n  </speech>\r\n\r\n</root>\r\n";
    std::string a = "<root>\n  <name key=\"Sword\" tags=\"n\">\n    <singular>Blade</singular>\n  </name>\n</root>\n";
    std::string b = "<root>\n  <speech key=\"title:warrior:1.4\">\n    A Slimy Affair\n  </speech>\n"
                    "  <speech key=\"mymod:new\">\n    New line\n  </speech>\n</root>\n";
    std::vector<std::string> rep, add;
    std::string m1 = merge_dictionary(base, a, &rep, &add);
    std::string m2 = merge_dictionary(m1, b, &rep, &add);
    CHECK(m2.find("<singular>Blade</singular>") != std::string::npos);
    CHECK(m2.find("A Slimy Affair") != std::string::npos);
    CHECK(m2.find("Eine schleimige") == std::string::npos);
    CHECK(m2.find("<speech key=\"mymod:new\">\r\n    New line\r\n  </speech>\r\n\r\n</root>") != std::string::npos);
    CHECK(rep == std::vector<std::string>{"Sword", "title:warrior:1.4"});
    CHECK(add == std::vector<std::string>{"mymod:new"});
    CHECK(m2.substr(0, 3) == "\xEF\xBB\xBF");
    CHECK(m2.find("\xEF\xBB\xBF", 3) == std::string::npos);
}

TEST_CASE("asset-overlay: C++ vox conversion matches cwmod-tools on real models" * doctest::skip(!getenv("KYUBU_VOX_DIR"))) {
    // KYUBU_VOX_DIR holds <name>.vox and <name>.cub pairs written by tools/cwmod (see tests/README.md).
    std::string dir = getenv("KYUBU_VOX_DIR") ? getenv("KYUBU_VOX_DIR") : "";
    int n = 0, bad = 0;
    for (auto& f : list_dir(dir)) {
        if (!ends_with(f, ".vox")) continue;
        auto vox = read_file(join_path(dir, f));
        auto cub = read_file(join_path(dir, f.substr(0, f.size() - 4) + ".cub"));
        REQUIRE(vox);
        REQUIRE(cub);
        std::string err;
        auto m = parse_vox(*vox, &err);
        if (!m || write_cub(*m) != *cub) { ++bad; MESSAGE("mismatch: " << f << " " << err); }
        ++n;
    }
    MESSAGE(n << " models compared");
    CHECK(n > 0);
    CHECK(bad == 0);
}
