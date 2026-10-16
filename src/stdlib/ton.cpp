#include "stdlib/ton.hpp"
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

} // namespace softplc
