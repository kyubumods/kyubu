#include "sounds.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

#include "particles.h"   // marker_times

namespace kanimator {

namespace {

namespace fs = std::filesystem;

uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}
void wr16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

WavInfo refused(const std::string& why) {
    WavInfo w;
    w.error = why;
    return w;
}

std::string lower(std::string s) {
    for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

// The paths here are UTF-8 (ImGui's text and the rig's names are).
fs::path u8_path(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string from_u8(const std::u8string& s) { return std::string(s.begin(), s.end()); }

// A folder compared with another: one kind of slash, no slash at the end, any case.
std::string folder_key(const std::string& f) {
    std::string k = lower(f);
    std::replace(k.begin(), k.end(), '\\', '/');
    while (k.size() > 1 && k.back() == '/') k.pop_back();
    return k;
}

}  // namespace

WavInfo parse_wav(const uint8_t* b, size_t size) {
    if (!b || size < 12 || std::memcmp(b, "RIFF", 4) != 0 || std::memcmp(b + 8, "WAVE", 4) != 0) return refused("not a RIFF/WAVE file");
    WavInfo w;
    bool have_fmt = false, have_data = false;
    size_t pos = 12;
    while (pos + 8 <= size) {
        const uint32_t n = rd32(b + pos + 4);
        const size_t body = pos + 8;
        const size_t avail = size - body;
        if (std::memcmp(b + pos, "fmt ", 4) == 0 && !have_fmt) {
            if (n < 16 || n > avail) return refused("the fmt chunk is cut short");
            w.fmt.assign(b + body, b + body + n);
            have_fmt = true;
        } else if (std::memcmp(b + pos, "data", 4) == 0 && !have_data) {
            w.data_offset = body;
            w.data_size = std::min<size_t>(n, avail);   // a file cut short keeps the samples it has
            have_data = true;
        }
        if (n > avail) break;
        pos = body + n + (n & 1);   // a chunk of an odd size is padded to an even one
    }
    if (!have_fmt) return refused("no fmt chunk");
    if (!have_data) return refused("no data chunk");
    const uint8_t* f = w.fmt.data();
    uint16_t tag = rd16(f);
    w.channels = rd16(f + 2);
    w.rate = rd32(f + 4);
    w.block_align = rd16(f + 12);
    w.bits = rd16(f + 14);
    if (tag == 0xFFFE) {
        if (w.fmt.size() < 40) return refused("a WAVE_FORMAT_EXTENSIBLE fmt chunk under 40 bytes");
        w.extensible = true;
        tag = rd16(f + 24);   // the sub-format GUID starts with the format's code
    }
    w.format = tag;
    char why[160];
    if (tag != 1 && tag != 3) {
        std::snprintf(why, sizeof why, "the sample format %u is neither PCM (1) nor float (3)", static_cast<unsigned>(tag));
        return refused(why);
    }
    if ((tag == 1 && w.bits != 8 && w.bits != 16 && w.bits != 24 && w.bits != 32) || (tag == 3 && w.bits != 32)) {
        std::snprintf(why, sizeof why, "%u-bit %s is not played (PCM: 8, 16, 24 or 32 bits; float: 32)", static_cast<unsigned>(w.bits),
                      tag == 1 ? "PCM" : "float");
        return refused(why);
    }
    if (w.channels < 1 || w.channels > kMaxSoundChannels) {
        std::snprintf(why, sizeof why, "%u channels (1 to %u are played)", static_cast<unsigned>(w.channels),
                      static_cast<unsigned>(kMaxSoundChannels));
        return refused(why);
    }
    if (w.rate < kMinSoundRate || w.rate > kMaxSoundRate) {
        std::snprintf(why, sizeof why, "%u frames a second (%u to %u are played)", static_cast<unsigned>(w.rate),
                      static_cast<unsigned>(kMinSoundRate), static_cast<unsigned>(kMaxSoundRate));
        return refused(why);
    }
    if (w.block_align != w.channels * (w.bits / 8)) {
        std::snprintf(why, sizeof why, "the block align %u is not %u channels of %u bits", static_cast<unsigned>(w.block_align),
                      static_cast<unsigned>(w.channels), static_cast<unsigned>(w.bits));
        return refused(why);
    }
    w.data_size -= w.data_size % w.block_align;
    if (w.data_size == 0) return refused("no samples");
    // The fmt the sound device is given: a WAVEFORMATEX (18 bytes, cbSize 0), or the whole WAVEFORMATEXTENSIBLE (40, cbSize 22).
    w.fmt.resize(w.extensible ? 40 : 18, 0);
    wr16(w.fmt.data() + 16, w.extensible ? 22 : 0);
    w.length_ms = static_cast<float>(static_cast<double>(w.data_size / w.block_align) * 1000.0 / w.rate);
    w.game_takes = !w.extensible && ((tag == 1 && w.bits == 16) || (tag == 3 && w.bits == 32));
    w.ok = true;
    return w;
}

std::string describe_wav(const WavInfo& w) {
    if (!w.ok) return w.error;
    char ch[24];
    if (w.channels == 1) std::snprintf(ch, sizeof ch, "mono");
    else if (w.channels == 2) std::snprintf(ch, sizeof ch, "stereo");
    else std::snprintf(ch, sizeof ch, "%u channels", static_cast<unsigned>(w.channels));
    char out[96];
    std::snprintf(out, sizeof out, "%u Hz %s %u-bit%s, %.2f s", static_cast<unsigned>(w.rate), ch, static_cast<unsigned>(w.bits),
                  w.format == 3 ? " float" : "", w.length_ms / 1000.0f);
    return out;
}

std::vector<SoundCue> plan_sounds(const Rig& rig, const Clip& clip, float length_ms, bool loop) {
    std::vector<SoundCue> out;
    if (!(length_ms > 0)) return out;
    for (size_t i = 0; i < rig.sounds.size(); ++i)
        for (float m : marker_times(clip, rig.sounds[i].at, length_ms)) {
            float t = m + rig.sounds[i].delay_ms;
            if (t > length_ms) {
                if (!loop) continue;
                t = std::fmod(t, length_ms);
            }
            out.push_back({t, i});
        }
    std::stable_sort(out.begin(), out.end(), [](const SoundCue& a, const SoundCue& b) { return a.t_ms < b.t_ms; });
    return out;
}

std::vector<size_t> sounds_due(const std::vector<SoundCue>& cues, float from_ms, float to_ms, bool wrapped, bool from_included) {
    std::vector<size_t> out;
    auto past_from = [&](float t) { return from_included ? t >= from_ms : t > from_ms; };
    if (!wrapped) {
        for (size_t i = 0; i < cues.size(); ++i)
            if (past_from(cues[i].t_ms) && cues[i].t_ms <= to_ms) out.push_back(i);
        return out;
    }
    for (size_t i = 0; i < cues.size(); ++i)
        if (past_from(cues[i].t_ms)) out.push_back(i);
    for (size_t i = 0; i < cues.size(); ++i)
        if (cues[i].t_ms <= to_ms) out.push_back(i);
    return out;
}

std::string clip_sound_folder(const std::string& clip_path) {
    std::string p = clip_path;
    std::replace(p.begin(), p.end(), '\\', '/');
    const std::string mid = "/assets/animations/";
    const size_t at = p.rfind(mid);
    if (at == std::string::npos || at == 0) return std::string();
    const std::string file = p.substr(at + mid.size());
    if (file.empty() || file.find('/') != std::string::npos) return std::string();
    const char sep = clip_path.find('\\') != std::string::npos ? '\\' : '/';
    return clip_path.substr(0, at) + sep + "assets" + sep + "sounds";
}

std::vector<std::string> sound_folders(const std::string& clip_path, const std::string& folder) {
    std::vector<std::string> out;
    std::vector<std::string> keys;
    for (const std::string& f : {clip_sound_folder(clip_path), folder}) {
        if (f.empty()) continue;
        const std::string k = folder_key(f);
        if (std::find(keys.begin(), keys.end(), k) != keys.end()) continue;
        keys.push_back(k);
        out.push_back(f);
    }
    return out;
}

std::string find_sound(const std::string& name, const std::vector<std::string>& folders) {
    if (name.empty()) return std::string();
    for (const std::string& f : folders) {
        const std::string p = f + "/" + name;
        std::error_code ec;
        if (fs::is_regular_file(u8_path(p), ec)) return p;
    }
    return std::string();
}

std::vector<std::string> list_sounds(const std::string& folder) {
    std::vector<std::string> out;
    if (folder.empty()) return out;
    std::error_code ec;
    for (fs::directory_iterator it(u8_path(folder), ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code fe;
        if (!it->is_regular_file(fe)) continue;
        const std::string name = from_u8(it->path().filename().u8string());
        if (name.size() > 4 && lower(name.substr(name.size() - 4)) == ".wav") out.push_back(name);
    }
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        const std::string la = lower(a), lb = lower(b);
        return la != lb ? la < lb : a < b;
    });
    return out;
}

bool read_sound_bytes(const std::string& path, std::vector<uint8_t>& out, std::string* why) {
    out.clear();
    std::error_code ec;
    const uintmax_t size = fs::file_size(u8_path(path), ec);
    if (ec) {
        if (why) *why = "cannot read " + path;
        return false;
    }
    if (size > kMaxSoundBytes) {
        if (why) *why = "over " + std::to_string(kMaxSoundBytes >> 20) + " MB: " + path;
        return false;
    }
    std::ifstream in(u8_path(path), std::ios::binary);
    out.resize(static_cast<size_t>(size));
    if (!in || (size && !in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size)))) {
        out.clear();
        if (why) *why = "cannot read " + path;
        return false;
    }
    return true;
}

std::shared_ptr<const LoadedSound> SoundCache::get(const std::string& path) {
    std::error_code ec;
    const fs::path p = u8_path(path);
    const uintmax_t size = fs::file_size(p, ec);
    const bool found = !ec;
    const int64_t time = found ? static_cast<int64_t>(fs::last_write_time(p, ec).time_since_epoch().count()) : 0;
    auto it = entries_.find(path);
    if (it != entries_.end() && found && it->second.size == size && it->second.time == time) return it->second.sound;
    auto s = std::make_shared<LoadedSound>();
    s->path = path;
    std::string why;
    if (read_sound_bytes(path, s->bytes, &why)) {
        s->info = parse_wav(s->bytes.data(), s->bytes.size());
    } else {
        s->info.error = why;
    }
    if (!s->info.ok) s->bytes.clear();
    entries_[path] = Entry{s, size, time};
    return s;
}

}  // namespace kanimator
