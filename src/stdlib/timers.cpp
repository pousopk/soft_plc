#include "stdlib/timers.hpp"
#include <algorithm>

namespace softplc {

void Ton::update(bool in, Duration preset, TimePoint now) {
    if (!in) {
        running_ = false;
        q_ = false;
        et_ = Duration{};
        return;
    }

    if (!running_) {
        // Rising edge: IN was false last time, so timing starts now.
        running_ = true;
        start_ = now;
    }

    et_ = std::min(now - start_, preset);
    q_ = et_ >= preset;
}

void Tof::update(bool in, Duration preset, TimePoint now) {
    if (in) {
        // While IN is on, Q is on and nothing is timing.
        timing_ = false;
        q_ = true;
        et_ = Duration{};
        return;
    }

    if (q_ && !timing_) {
        // Falling edge: IN just went off while Q was on, so the delay starts.
        timing_ = true;
        start_ = now;
    }

    if (timing_) {
        et_ = std::min(now - start_, preset);
        if (et_ >= preset) {
            timing_ = false;
            q_ = false; // ET stays at PT until IN comes back on
        }
    }
}

void Tp::update(bool in, Duration preset, TimePoint now) {
    const bool risingEdge = in && !previousIn_;
    previousIn_ = in;

    if (!pulsing_) {
        if (risingEdge) {
            pulsing_ = true; // edges during a pulse never get here, so they're ignored
            start_ = now;
        } else if (!in) {
            et_ = Duration{}; // after a pulse, ET holds at PT until IN goes off
        }
    }

    if (pulsing_) {
        et_ = std::min(now - start_, preset);
        if (et_ >= preset) {
            pulsing_ = false;
        }
    }
    q_ = pulsing_;
}

} // namespace softplc
