#pragma once
#include <chrono>

namespace softplc {

// PLC time is monotonic: steady_clock never jumps when the system clock is
// adjusted, which is what timers and cycle-time measurement need.
using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Duration = Clock::duration;

} // namespace softplc
