// The Animator's play controls (openspec add-ability-animation, task 6.3): the scrub time, play, pause, loop and the speed. Pure and
// header-only: the window calls advance() once per frame and reads time_ms; tests/unit/test_animator_view.cpp runs it without one.
#pragma once
#include <algorithm>
#include <cmath>

#include "animation_eval.h"

namespace kanimator {

constexpr float kMinSpeed = 0.1f;   // the spec: 0.1x to 2x
constexpr float kMaxSpeed = 2.0f;

struct Playback {
    float time_ms = 0;     // the scrub time, 0 to length_ms
    float length_ms = 0;   // the clip's duration; 0 when there is no clip (nothing to play)
    bool playing = false;
    bool loop = true;
    float speed = 1;

    void set_length(float ms) {
        length_ms = std::max(ms, 0.0f);
        time_ms = std::min(time_ms, length_ms);
        if (length_ms <= 0) playing = false;
    }
    void set_speed(float s) { speed = std::clamp(s, kMinSpeed, kMaxSpeed); }
    // The scrub bar: a time, held inside the clip. Playing goes on from there.
    void scrub(float ms) { time_ms = std::clamp(ms, 0.0f, length_ms); }
    void play() {
        if (length_ms <= 0) return;
        if (!loop && time_ms >= length_ms) time_ms = 0;   // play at the end starts again
        playing = true;
    }
    void pause() { playing = false; }
    // `dt_ms` of real time went by. Moves the time by dt * speed when playing; at the end it wraps (loop) or stops there.
    void advance(float dt_ms) {
        if (!playing || length_ms <= 0 || dt_ms <= 0) return;
        time_ms += dt_ms * speed;
        if (time_ms < length_ms) return;
        if (loop) {
            time_ms = std::fmod(time_ms, length_ms);
        } else {
            time_ms = length_ms;
            playing = false;
        }
    }
};

// What the evaluator is given for the scrub time: the clip is at `time_ms` of a run that lasts the whole clip (so a blend out is
// seen at the end, as in `kanim sample`).
inline kyubu::game::anim::Timing view_timing(const Playback& p) {
    kyubu::game::anim::Timing t;
    t.elapsed_ms = p.time_ms;
    t.duration_ms = p.length_ms;
    t.run_ms = p.length_ms;
    return t;
}

}  // namespace kanimator
