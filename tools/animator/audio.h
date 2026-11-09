// The Animator's sound device: XAudio2, as the game's own engine. xaudio2_9.dll (Windows 10 and later, and Wine), else
// xaudio2_8.dll, is loaded from System32 when the window opens, so the program still starts where neither is (the Sounds panel then
// says why it is silent). It plays the files of sounds.h, each on a voice of its own, so sounds overlap as they do in the game.
// Not pure: only the window (app.cpp) uses it.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "sounds.h"

namespace kanimator {

constexpr size_t kMaxVoices = 32;   // one more than this stops the oldest

class AudioOut {
public:
    AudioOut();
    ~AudioOut();
    AudioOut(const AudioOut&) = delete;
    AudioOut& operator=(const AudioOut&) = delete;

    bool open();                           // false: why() says
    void close();
    bool ok() const;
    const std::string& why() const { return why_; }     // why there is no device, when !ok()
    const std::string& name() const { return name_; }   // "XAudio2 2.9" or "XAudio2 2.8"
    // Plays `s` at `volume` (1: as the file is) and `pitch` (kMinSoundPitch to kMaxSoundPitch: the speed it is played at). False and
    // the reason when it cannot.
    bool play(const std::shared_ptr<const LoadedSound>& s, float volume, float pitch, std::string* why);
    void stop_all();
    // Frees the voices that have finished. Once a frame.
    void tick();
    size_t playing() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string why_, name_;
};

}  // namespace kanimator
