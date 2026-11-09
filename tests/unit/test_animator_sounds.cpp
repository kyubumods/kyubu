// Unit tests of the Animator's sounds (tools/animator/sounds.h) and the rig's `[[sound]]` table: what is read and what is refused,
// the .wav reader (the formats played, the fmt handed to the device, a file cut short, odd chunks), the cues at the clip's markers
// (a delay, a loop's wrap, the order), the cues a step of the playback passes, the folders a file is looked up in, and the cache
// that reads a file again when it changes. No sound device: audio.h is the window's.
#include "doctest.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "animation_load.h"
#include "rig.h"
#include "sounds.h"

using namespace kanimator;
namespace anim = kyubu::game::anim;
namespace fs = std::filesystem;

namespace {

const char* kNodes = "name = \"t\"\n[[node]]\nname = \"root\"\n";

RigLoad load(const std::string& more) { return parse_rig(std::string(kNodes) + more, "t.rig.toml"); }

std::string first_code(const std::string& more) {
    RigLoad r = load(more);
    return r.errors.empty() ? std::string() : r.errors.front().code;
}

std::string first_path(const std::string& more) {
    RigLoad r = load(more);
    return r.errors.empty() ? std::string() : r.errors.front().path;
}

// A clip of 1 s with no motion and markers [(t 0..1, name)].
anim::Clip marker_clip(const std::vector<std::pair<float, std::string>>& marks) {
    std::string text = "duration = 1000\nend = \"hold\"\nblend_in = 0\nblend_out = 0\n"
                       "[[track]]\npart = \"tail\"\nchannel = \"pos\"\nmode = \"add\"\nkeys = [{ t = 0, v = [0, 0, 0] }, { t = 1, v = [0, 0, 0] }]\n";
    for (const auto& m : marks) text += "[[marker]]\nt = " + std::to_string(m.first) + "\nname = \"" + m.second + "\"\n";
    anim::ClipLoad c = anim::parse_clip(text, "c.anim.toml");
    for (const ClipError& e : c.errors) MESSAGE(anim::format_error(e));
    REQUIRE(c.ok);
    return c.clip;
}

void put16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v));
    b.push_back(static_cast<uint8_t>(v >> 8));
}
void put32(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void tag(std::vector<uint8_t>& b, const char* t) { b.insert(b.end(), t, t + 4); }

struct WavSpec {
    uint16_t format = 1;
    uint16_t channels = 1;
    uint32_t rate = 22050;
    uint16_t bits = 16;
    int block_align = -1;          // -1: channels * bits / 8
    uint32_t frames = 2205;        // 0.1 s at 22050
    bool extensible = false;
    uint16_t sub_format = 1;
    uint32_t fmt_size = 16;        // 16 or 18 (40 for extensible)
    std::string before_data;       // an odd chunk's body put before the data chunk (as a LIST), when not empty
    int64_t data_claim = -1;       // the data chunk's size field; -1: the real size
};

std::vector<uint8_t> wav(const WavSpec& s) {
    const uint16_t align = static_cast<uint16_t>(s.block_align >= 0 ? s.block_align : s.channels * (s.bits / 8));
    std::vector<uint8_t> b;
    tag(b, "RIFF");
    put32(b, 0);   // the RIFF size is not read
    tag(b, "WAVE");
    tag(b, "fmt ");
    const uint32_t fmt_size = s.extensible ? 40 : s.fmt_size;
    put32(b, fmt_size);
    put16(b, s.extensible ? 0xFFFE : s.format);
    put16(b, s.channels);
    put32(b, s.rate);
    put32(b, s.rate * align);
    put16(b, align);
    put16(b, s.bits);
    if (s.extensible) {
        put16(b, 22);
        put16(b, s.bits);   // valid bits
        put32(b, 0);        // channel mask
        put16(b, s.sub_format);
        static const uint8_t rest[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
        b.insert(b.end(), rest, rest + 14);
    } else if (fmt_size == 18) {
        put16(b, 0);
    }
    if (!s.before_data.empty()) {
        tag(b, "LIST");
        put32(b, static_cast<uint32_t>(s.before_data.size()));
        b.insert(b.end(), s.before_data.begin(), s.before_data.end());
        if (s.before_data.size() & 1) b.push_back(0);
    }
    tag(b, "data");
    const uint32_t bytes = s.frames * align;
    put32(b, s.data_claim >= 0 ? static_cast<uint32_t>(s.data_claim) : bytes);
    for (uint32_t i = 0; i < bytes; ++i) b.push_back(static_cast<uint8_t>(i * 7));
    return b;
}

WavInfo read(const std::vector<uint8_t>& b) { return parse_wav(b.data(), b.size()); }

struct TempDir {
    fs::path path;
    explicit TempDir(const std::string& name) {
        path = fs::temp_directory_path() / ("kyubu-sounds-" + name);
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    std::string str() const { return path.string(); }
    void write(const std::string& rel, const std::vector<uint8_t>& bytes) const {
        const fs::path p = path / rel;
        fs::create_directories(p.parent_path());
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
};

}  // namespace

TEST_CASE("animator sounds: the rig's [[sound]] table") {
    RigLoad r = load("[[sound]]\nfile = \"hit.wav\"\nat = \"strike\"\n"
                     "[[sound]]\nfile = \"Explosion.WAV\"\nat = \"boom\"\nvolume = 0.5\npitch = 1.5\ndelay_ms = 120\n");
    for (const ClipError& e : r.errors) MESSAGE(anim::format_error(e));
    REQUIRE(r.ok);
    REQUIRE(r.rig.sounds.size() == 2);
    const RigSound& a = r.rig.sounds[0];
    CHECK(a.file == "hit.wav");
    CHECK(a.at == "strike");
    CHECK(a.volume == 1);
    CHECK(a.pitch == 1);
    CHECK(a.delay_ms == 0);
    const RigSound& b = r.rig.sounds[1];
    CHECK(b.file == "Explosion.WAV");
    CHECK(b.volume == doctest::Approx(0.5));
    CHECK(b.pitch == doctest::Approx(1.5));
    CHECK(b.delay_ms == doctest::Approx(120));
    CHECK(b.line == 7);   // its [[sound]] header
    CHECK_FALSE(r.rig.has_effects());   // a sound needs no [particles] table
    CHECK(r.rig.parts_used() == 0);

    CHECK(first_code("[[sound]]\nat = \"x\"\n") == "missing_field");
    CHECK(first_path("[[sound]]\nat = \"x\"\n") == "sound[0].file");
    CHECK(first_code("[[sound]]\nfile = \"hit.wav\"\n") == "missing_field");
    CHECK(first_path("[[sound]]\nfile = \"hit.wav\"\n") == "sound[0].at");
    CHECK(first_code("[[sound]]\nfile = \"hit.ogg\"\nat = \"x\"\n") == "bad_value");
    CHECK(first_code("[[sound]]\nfile = \"sub/hit.wav\"\nat = \"x\"\n") == "bad_value");
    CHECK(first_code("[[sound]]\nfile = 3\nat = \"x\"\n") == "bad_type");
    CHECK(first_code("[[sound]]\nfile = \"hit.wav\"\nat = \"\"\n") == "bad_value");
    CHECK(first_code("[[sound]]\nfile = \"hit.wav\"\nat = \"x\"\nvolume = 5\n") == "out_of_range");
    CHECK(first_code("[[sound]]\nfile = \"hit.wav\"\nat = \"x\"\npitch = 0.1\n") == "out_of_range");
    CHECK(first_code("[[sound]]\nfile = \"hit.wav\"\nat = \"x\"\ndelay_ms = -1\n") == "out_of_range");
    CHECK(first_code("[[sound]]\nfile = \"hit.wav\"\nat = \"x\"\nloud = true\n") == "unknown_key");
    RigLoad top = parse_rig("name = \"t\"\nsound = 3\n[[node]]\nname = \"root\"\n", "t.rig.toml");   // before [[node]]: the rig's own key
    REQUIRE_FALSE(top.errors.empty());
    CHECK(top.errors.front().code == "bad_type");
    std::string many;
    for (int i = 0; i <= kMaxSounds; ++i) many += "[[sound]]\nfile = \"hit.wav\"\nat = \"x\"\n";
    CHECK(first_code(many) == "too_many_nodes");
}

TEST_CASE("animator sounds: a sound file's name") {
    CHECK(valid_sound_file("hit.wav"));
    CHECK(valid_sound_file("step-water.wav"));
    CHECK(valid_sound_file("Explosion.WAV"));
    CHECK(valid_sound_file("a.b_c-1.wav"));
    CHECK_FALSE(valid_sound_file(".wav"));
    CHECK_FALSE(valid_sound_file("hit"));
    CHECK_FALSE(valid_sound_file("hit.wav.txt"));
    CHECK_FALSE(valid_sound_file("hit wav.wav"));
    CHECK_FALSE(valid_sound_file("../hit.wav"));
    CHECK_FALSE(valid_sound_file(std::string(kMaxSoundFile - 3, 'a') + ".wav"));
    CHECK(valid_sound_file(std::string(kMaxSoundFile - 4, 'a') + ".wav"));
}

TEST_CASE("animator sounds: the .wav reader") {
    SUBCASE("16-bit PCM mono, a 16-byte fmt") {
        const std::vector<uint8_t> b = wav({});
        const WavInfo w = read(b);
        REQUIRE(w.ok);
        CHECK(w.format == 1);
        CHECK_FALSE(w.extensible);
        CHECK(w.channels == 1);
        CHECK(w.rate == 22050);
        CHECK(w.bits == 16);
        CHECK(w.block_align == 2);
        CHECK(w.data_size == 2205 * 2);
        CHECK(w.data_offset == 44);
        CHECK(w.length_ms == doctest::Approx(100));
        CHECK(w.game_takes);
        REQUIRE(w.fmt.size() == 18);   // a WAVEFORMATEX, cbSize 0
        CHECK(w.fmt[16] == 0);
        CHECK(w.fmt[17] == 0);
        CHECK(describe_wav(w) == "22050 Hz mono 16-bit, 0.10 s");
    }
    SUBCASE("the other formats played") {
        WavSpec f;
        f.format = 3;
        f.bits = 32;
        f.channels = 2;
        f.rate = 48000;
        f.frames = 48000;
        f.fmt_size = 18;
        const WavInfo fw = read(wav(f));
        REQUIRE(fw.ok);
        CHECK(fw.game_takes);
        CHECK(fw.block_align == 8);
        CHECK(fw.length_ms == doctest::Approx(1000));
        CHECK(describe_wav(fw) == "48000 Hz stereo 32-bit float, 1.00 s");
        WavSpec p24;
        p24.bits = 24;
        const WavInfo w24 = read(wav(p24));
        REQUIRE(w24.ok);
        CHECK_FALSE(w24.game_takes);   // played here, refused by the loader
        WavSpec p8;
        p8.bits = 8;
        CHECK(read(wav(p8)).ok);
        WavSpec x;
        x.extensible = true;
        const WavInfo wx = read(wav(x));
        REQUIRE(wx.ok);
        CHECK(wx.extensible);
        CHECK(wx.format == 1);
        CHECK_FALSE(wx.game_takes);
        REQUIRE(wx.fmt.size() == 40);
        CHECK(wx.fmt[16] == 22);
        CHECK(wx.fmt[0] == 0xFE);
    }
    SUBCASE("an odd chunk before the data, and a data chunk cut short") {
        WavSpec s;
        s.before_data = "abc";   // 3 bytes and a pad byte
        const WavInfo w = read(wav(s));
        REQUIRE(w.ok);
        CHECK(w.data_offset == 44 + 12);
        CHECK(w.data_size == 2205 * 2);
        WavSpec cut;
        cut.data_claim = 1000001;   // past the end: the samples there are kept, in whole frames
        std::vector<uint8_t> b = wav(cut);
        b.pop_back();               // half a frame at the end
        const WavInfo wc = read(b);
        REQUIRE(wc.ok);
        CHECK(wc.data_size == 2204 * 2);
    }
    SUBCASE("what is refused") {
        const std::vector<uint8_t> good = wav({});
        CHECK_FALSE(parse_wav(nullptr, 0).ok);
        CHECK(read(std::vector<uint8_t>(good.begin(), good.begin() + 8)).error == "not a RIFF/WAVE file");
        std::vector<uint8_t> riff = good;
        std::memcpy(riff.data() + 8, "AVI ", 4);
        CHECK(read(riff).error == "not a RIFF/WAVE file");
        CHECK(read(std::vector<uint8_t>(good.begin(), good.begin() + 12)).error == "no fmt chunk");
        CHECK(read(std::vector<uint8_t>(good.begin(), good.begin() + 36)).error == "no data chunk");
        CHECK(read(std::vector<uint8_t>(good.begin(), good.begin() + 30)).error == "the fmt chunk is cut short");
        WavSpec s;
        s.bits = 12;
        CHECK(read(wav(s)).error.find("12-bit PCM") != std::string::npos);
        s = {};
        s.format = 3;
        s.bits = 64;
        CHECK_FALSE(read(wav(s)).ok);
        s = {};
        s.format = 2;   // ADPCM
        CHECK(read(wav(s)).error.find("format 2") != std::string::npos);
        s = {};
        s.channels = 0;
        s.block_align = 0;
        CHECK(read(wav(s)).error.find("channels") != std::string::npos);
        s = {};
        s.rate = 500;
        CHECK(read(wav(s)).error.find("frames a second") != std::string::npos);
        s = {};
        s.block_align = 3;
        CHECK(read(wav(s)).error.find("block align") != std::string::npos);
        s = {};
        s.frames = 0;
        CHECK(read(wav(s)).error == "no samples");
        s = {};
        s.extensible = true;
        s.sub_format = 2;
        CHECK_FALSE(read(wav(s)).ok);
    }
}

TEST_CASE("animator sounds: the cues at the markers") {
    RigLoad r = load("[[sound]]\nfile = \"a.wav\"\nat = \"hit\"\n"
                     "[[sound]]\nfile = \"b.wav\"\nat = \"hit\"\ndelay_ms = 100\n"
                     "[[sound]]\nfile = \"c.wav\"\nat = \"none\"\n"
                     "[[sound]]\nfile = \"d.wav\"\nat = \"end\"\ndelay_ms = 300\n");
    REQUIRE(r.ok);
    const anim::Clip clip = marker_clip({{0.25f, "hit"}, {0.75f, "hit"}, {0.9f, "end"}});
    const std::vector<SoundCue> once = plan_sounds(r.rig, clip, 1000, false);
    REQUIRE(once.size() == 4);   // d's cue at 1200 is past the end and dropped
    CHECK(once[0].t_ms == doctest::Approx(250));
    CHECK(once[0].sound == 0);
    CHECK(once[1].t_ms == doctest::Approx(350));
    CHECK(once[1].sound == 1);
    CHECK(once[2].t_ms == doctest::Approx(750));
    CHECK(once[3].t_ms == doctest::Approx(850));
    const std::vector<SoundCue> looped = plan_sounds(r.rig, clip, 1000, true);
    REQUIRE(looped.size() == 5);
    CHECK(looped[0].t_ms == doctest::Approx(200));   // d's, in the next lap
    CHECK(looped[0].sound == 3);
    // Two sounds at one time keep the rig's order.
    RigLoad two = load("[[sound]]\nfile = \"a.wav\"\nat = \"hit\"\n[[sound]]\nfile = \"b.wav\"\nat = \"hit\"\n");
    REQUIRE(two.ok);
    const std::vector<SoundCue> tie = plan_sounds(two.rig, marker_clip({{0.5f, "hit"}}), 1000, false);
    REQUIRE(tie.size() == 2);
    CHECK(tie[0].sound == 0);
    CHECK(tie[1].sound == 1);
    CHECK(plan_sounds(r.rig, clip, 0, true).empty());
    CHECK(plan_sounds(r.rig, marker_clip({}), 1000, true).empty());
}

TEST_CASE("animator sounds: the cues a step passes") {
    std::vector<SoundCue> cues = {{0, 0}, {250, 1}, {500, 2}, {1000, 3}};
    using V = std::vector<size_t>;
    CHECK(sounds_due(cues, 0, 250, false, false) == V{1});      // (0, 250]: the cue at the start of the step is the last step's
    CHECK(sounds_due(cues, 0, 250, false, true) == V{0, 1});    // the first step after Play: [0, 250]
    CHECK(sounds_due(cues, 250, 499, false, false).empty());
    CHECK(sounds_due(cues, 499, 500, false, false) == V{2});
    CHECK(sounds_due(cues, 980, 1000, false, false) == V{3});   // a clip that stops at its end hears the cue there
    CHECK(sounds_due(cues, 980, 20, true, false) == V{3, 0});   // a loop's wrap: (980, end], then [0, 20]
    CHECK(sounds_due(cues, 600, 300, true, false) == V{3, 0, 1});
    CHECK(sounds_due({}, 0, 1000, false, true).empty());
}

TEST_CASE("animator sounds: the folders") {
    CHECK(clip_sound_folder("D:/mods/fire/assets/animations/cast.anim.toml") == "D:/mods/fire/assets/sounds");
    CHECK(clip_sound_folder("D:\\mods\\fire\\assets\\animations\\cast.anim.toml") == "D:\\mods\\fire\\assets\\sounds");
    CHECK(clip_sound_folder("D:/mods/fire/assets/animations/old/cast.anim.toml").empty());
    CHECK(clip_sound_folder("D:/work/cast.anim.toml").empty());
    CHECK(clip_sound_folder("").empty());
    using V = std::vector<std::string>;
    CHECK(sound_folders("D:/m/x/assets/animations/a.anim.toml", "D:/snd") == V{"D:/m/x/assets/sounds", "D:/snd"});
    CHECK(sound_folders("D:/m/x/assets/animations/a.anim.toml", "d:\\m\\x\\assets\\sounds\\") == V{"D:/m/x/assets/sounds"});
    CHECK(sound_folders("", "D:/snd") == V{"D:/snd"});
    CHECK(sound_folders("D:/work/a.anim.toml", "").empty());

    TempDir mod("mod"), lib("lib");
    const std::vector<uint8_t> b = wav({});
    mod.write("hit.wav", b);
    lib.write("hit.wav", b);
    lib.write("Blade1.WAV", b);
    lib.write("arrow.wav", b);
    lib.write("notes.txt", {1, 2, 3});
    lib.write("sub/deep.wav", b);
    CHECK(list_sounds(lib.str()) == V{"arrow.wav", "Blade1.WAV", "hit.wav"});
    CHECK(list_sounds((lib.path / "missing").string()).empty());
    CHECK(list_sounds("").empty());
    CHECK(find_sound("hit.wav", {mod.str(), lib.str()}) == mod.str() + "/hit.wav");   // the mod's own first
    CHECK(find_sound("arrow.wav", {mod.str(), lib.str()}) == lib.str() + "/arrow.wav");
    CHECK_FALSE(find_sound("blade1.wav", {mod.str(), lib.str()}).empty());              // any case, as Windows
    CHECK(find_sound("none.wav", {mod.str(), lib.str()}).empty());
    CHECK(find_sound("sub", {lib.str()}).empty());                                       // a folder is not a sound
}

TEST_CASE("animator sounds: the cache reads a changed file again") {
    TempDir d("cache");
    d.write("a.wav", wav({}));
    const std::string path = d.str() + "/a.wav";
    SoundCache cache;
    const std::shared_ptr<const LoadedSound> first = cache.get(path);
    REQUIRE(first->info.ok);
    CHECK(first->bytes.size() == 44 + 2205 * 2);
    CHECK(cache.get(path) == first);
    WavSpec longer;
    longer.frames = 4410;
    d.write("a.wav", wav(longer));
    const std::shared_ptr<const LoadedSound> second = cache.get(path);
    CHECK(second != first);
    CHECK(second->info.length_ms == doctest::Approx(200));
    CHECK(first->info.length_ms == doctest::Approx(100));   // a voice that holds the old one keeps its samples
    d.write("bad.wav", {1, 2, 3});
    const std::shared_ptr<const LoadedSound> bad = cache.get(d.str() + "/bad.wav");
    CHECK_FALSE(bad->info.ok);
    CHECK(bad->bytes.empty());
    const std::shared_ptr<const LoadedSound> gone = cache.get(d.str() + "/gone.wav");
    CHECK_FALSE(gone->info.ok);
    CHECK(gone->info.error.find("cannot read") != std::string::npos);
    CHECK(cache.size() == 3);
}
