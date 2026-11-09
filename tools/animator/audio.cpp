#include "audio.h"

#include <windows.h>
#include <xaudio2.h>

#include <algorithm>
#include <cstdio>

#ifndef LOAD_LIBRARY_SEARCH_SYSTEM32
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x00000800
#endif

namespace kanimator {

namespace {

using CreateFn = HRESULT(WINAPI*)(IXAudio2**, UINT32, UINT32);
constexpr UINT32 kProcessor1 = 0x1;           // XAUDIO2_DEFAULT_PROCESSOR of 2.8 and 2.9 (the header's own value is 2.7's)
constexpr float kMaxVoiceVolume = 16;          // the panel's volume times a sound's: far past what is useful, short of a blast

std::string hex(HRESULT hr) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%08lX", static_cast<unsigned long>(hr));
    return b;
}

}  // namespace

struct AudioOut::Impl {
    HMODULE dll = nullptr;
    IXAudio2* xa = nullptr;
    IXAudio2MasteringVoice* master = nullptr;
    struct Voice {
        IXAudio2SourceVoice* v = nullptr;
        std::shared_ptr<const LoadedSound> s;   // the samples stay while the voice reads them
    };
    std::vector<Voice> voices;                  // the oldest first
};

AudioOut::AudioOut() : impl_(std::make_unique<Impl>()), why_("not opened") {}

AudioOut::~AudioOut() { close(); }

bool AudioOut::ok() const { return impl_->master != nullptr; }

bool AudioOut::open() {
    if (ok()) return true;
    close();
    struct Candidate {
        const wchar_t* dll;
        const char* label;
    };
    const Candidate list[] = {{L"xaudio2_9.dll", "XAudio2 2.9"}, {L"xaudio2_8.dll", "XAudio2 2.8"}};
    why_ = "neither xaudio2_9.dll nor xaudio2_8.dll is in System32";
    for (const Candidate& c : list) {
        HMODULE m = LoadLibraryExW(c.dll, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!m) continue;
        auto create = reinterpret_cast<CreateFn>(reinterpret_cast<void*>(GetProcAddress(m, "XAudio2Create")));
        IXAudio2* xa = nullptr;
        HRESULT hr = create ? create(&xa, 0, kProcessor1) : E_NOINTERFACE;
        if (FAILED(hr) || !xa) {
            why_ = std::string(c.label) + ": XAudio2Create failed (" + hex(hr) + ")";
            FreeLibrary(m);
            continue;
        }
        IXAudio2MasteringVoice* mv = nullptr;
        hr = xa->CreateMasteringVoice(&mv);
        if (FAILED(hr) || !mv) {
            why_ = "no sound output device (" + hex(hr) + ")";   // another version would not find one either
            xa->Release();
            FreeLibrary(m);
            return false;
        }
        impl_->dll = m;
        impl_->xa = xa;
        impl_->master = mv;
        name_ = c.label;
        why_.clear();
        return true;
    }
    return false;
}

void AudioOut::close() {
    stop_all();
    if (impl_->master) {
        impl_->master->DestroyVoice();
        impl_->master = nullptr;
    }
    if (impl_->xa) {
        impl_->xa->Release();
        impl_->xa = nullptr;
    }
    if (impl_->dll) {
        FreeLibrary(impl_->dll);
        impl_->dll = nullptr;
    }
    name_.clear();
}

bool AudioOut::play(const std::shared_ptr<const LoadedSound>& s, float volume, float pitch, std::string* why) {
    auto fail = [&](const std::string& w) {
        if (why) *why = w;
        return false;
    };
    if (!ok()) return fail(why_);
    if (!s) return fail("no sound");
    const WavInfo& w = s->info;
    if (!w.ok) return fail(w.error);
    if (w.fmt.size() < sizeof(WAVEFORMATEX) || w.data_offset + w.data_size > s->bytes.size()) return fail("the sound was not read whole");
    tick();
    while (impl_->voices.size() >= kMaxVoices) {
        impl_->voices.front().v->DestroyVoice();
        impl_->voices.erase(impl_->voices.begin());
    }
    IXAudio2SourceVoice* v = nullptr;
    HRESULT hr = impl_->xa->CreateSourceVoice(&v, reinterpret_cast<const WAVEFORMATEX*>(w.fmt.data()), 0, kMaxSoundPitch);
    if (FAILED(hr) || !v) return fail("the device refused its format (" + hex(hr) + ")");
    XAUDIO2_BUFFER buf = {};
    buf.Flags = XAUDIO2_END_OF_STREAM;
    buf.AudioBytes = static_cast<UINT32>(w.data_size);
    buf.pAudioData = s->bytes.data() + w.data_offset;
    hr = v->SubmitSourceBuffer(&buf);
    if (SUCCEEDED(hr)) {
        v->SetVolume(std::clamp(volume, 0.0f, kMaxVoiceVolume));
        v->SetFrequencyRatio(std::clamp(pitch, kMinSoundPitch, kMaxSoundPitch));
        hr = v->Start(0);
    }
    if (FAILED(hr)) {
        v->DestroyVoice();
        return fail("the device did not play it (" + hex(hr) + ")");
    }
    impl_->voices.push_back({v, s});
    return true;
}

void AudioOut::stop_all() {
    for (Impl::Voice& v : impl_->voices) v.v->DestroyVoice();
    impl_->voices.clear();
}

void AudioOut::tick() {
    std::vector<Impl::Voice>& vs = impl_->voices;
    for (size_t i = 0; i < vs.size();) {
        XAUDIO2_VOICE_STATE st = {};
        vs[i].v->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if (st.BuffersQueued == 0) {
            vs[i].v->DestroyVoice();
            vs.erase(vs.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

size_t AudioOut::playing() const { return impl_->voices.size(); }

}  // namespace kanimator
