// The Animator's sounds: the `.wav` files a rig's `[[sound]]`s name (rig.h), the times the playback plays them (each marker of a
// sound's `at`, plus its delay, as a mod plays a sound from an ability's step), and the folders the files are looked up in. Pure: no
// windows.h and no XAudio2 (audio.h plays them), so tests/unit/test_animator_sounds.cpp runs it without a sound device.
#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "rig.h"

namespace kanimator {

constexpr size_t kMaxSoundBytes = 64u << 20;   // a bigger .wav is refused (the game's are well under 1 MB)
constexpr uint16_t kMaxSoundChannels = 8;
constexpr uint32_t kMinSoundRate = 1000, kMaxSoundRate = 200000;   // frames a second: XAudio2's own bounds

// What a .wav file holds, read from its bytes.
struct WavInfo {
    bool ok = false;
    std::string error;                     // why it cannot be played, when !ok
    uint16_t format = 0;                   // 1: PCM, 3: IEEE float (a WAVE_FORMAT_EXTENSIBLE file: its sub-format)
    bool extensible = false;               // the fmt chunk is WAVE_FORMAT_EXTENSIBLE (0xFFFE)
    uint16_t channels = 0;
    uint32_t rate = 0;                     // frames a second
    uint16_t bits = 0;                     // per sample
    uint16_t block_align = 0;              // bytes a frame
    std::vector<uint8_t> fmt;              // the fmt chunk's body, at least 18 bytes (cbSize 0 added to a 16-byte one): a WAVEFORMATEX
    size_t data_offset = 0, data_size = 0; // the samples, in the file's bytes: whole frames
    float length_ms = 0;                   // at pitch 1
    bool game_takes = false;               // the loader serves it to the game: 16-bit PCM or 32-bit float, not extensible (validate_wav)
};

// Reads the RIFF/WAVE header, the fmt chunk and the data chunk. PCM of 8, 16, 24 or 32 bits and float of 32 bits, 1 to 8 channels,
// 1000 to 200000 frames a second. A data chunk that runs past the file's end is cut to it (and to whole frames).
WavInfo parse_wav(const uint8_t* bytes, size_t size);
// "22050 Hz mono 16-bit, 0.25 s": the format of a file that parsed, for a tooltip.
std::string describe_wav(const WavInfo& w);

// One time the playback plays a sound.
struct SoundCue {
    float t_ms = 0;                        // in the clip, 0 to its length
    size_t sound = 0;                      // its index in Rig::sounds
};

// The cues of the rig's sounds over a clip of `length_ms`: each marker of a sound's `at`, plus its `delay_ms`. A cue past the end
// is heard in the next lap when the playback loops (`loop`: its time is taken modulo the length) and dropped when it does not. In
// order of time, then of the sounds' order in the rig.
std::vector<SoundCue> plan_sounds(const Rig& rig, const Clip& clip, float length_ms, bool loop);

// The cues the playback passed going from `from_ms` to `to_ms`: those in (from, to], or in [from, to] when `from_included` (the
// first step after Play was pressed, so a cue at the time it starts from is heard, as the game tells a marker at 0 to the first
// step). `wrapped`: the playback went past the end and on from 0, so the span is (from, end] and then [0, to]. Indexes into
// `cues`, in the order they were passed.
std::vector<size_t> sounds_due(const std::vector<SoundCue>& cues, float from_ms, float to_ms, bool wrapped, bool from_included);

// The clip's mod's `assets/sounds` folder when the clip is `<mod>/assets/animations/<name>.anim.toml`, else empty.
std::string clip_sound_folder(const std::string& clip_path);
// The folders a sound is looked for in, in order: the clip's mod's sounds (a mod's own file is served before the game's, as the
// loader does), then `folder` (the Sounds panel's). Empty ones are left out; one that is given twice is kept once.
std::vector<std::string> sound_folders(const std::string& clip_path, const std::string& folder);
// The path of the file `name` in the first of `folders` that has it (the name in any case, as Windows compares them), or empty.
std::string find_sound(const std::string& name, const std::vector<std::string>& folders);
// The .wav files directly in `folder` (the extension in any case), sorted by name in any case; empty when it cannot be read.
std::vector<std::string> list_sounds(const std::string& folder);
// The whole file, false when it cannot be read or is over kMaxSoundBytes. The path is UTF-8.
bool read_sound_bytes(const std::string& path, std::vector<uint8_t>& out, std::string* why);

// A sound read from its file, held by the voices that play it (audio.h).
struct LoadedSound {
    std::string path;
    std::vector<uint8_t> bytes;
    WavInfo info;                          // not ok, with the reason, when the file could not be read or is not a .wav played
};

// The sounds read so far, each read again when its file changes (its size or its time), so a .wav made again is heard as it is now.
class SoundCache {
public:
    // The sound at `path` (UTF-8). Never null.
    std::shared_ptr<const LoadedSound> get(const std::string& path);
    void clear() { entries_.clear(); }
    size_t size() const { return entries_.size(); }

private:
    struct Entry {
        std::shared_ptr<const LoadedSound> sound;
        uintmax_t size = 0;
        int64_t time = 0;
    };
    std::map<std::string, Entry> entries_;
};

}  // namespace kanimator
