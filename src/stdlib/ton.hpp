#pragma once
#include "core/plc_time.hpp"

namespace softplc {

// IEC 61131-3 on-delay timer (TON).
//
// Call update() once per scan cycle, like calling a TON instance in a PLC
// program. Each Ton object is one timer instance and remembers its own state
// between cycles.
//
//   IN  ___/‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾\______
//   Q   _________/‾‾‾‾‾‾‾‾‾\______
//   ET  ___/‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾\______   (ramps from 0 up to PT, then holds at PT)
//          |<-- PT -->|
class Ton {
public:
    void update(bool in, Duration preset, TimePoint now);

    bool q() const { return q_; }         // output: IN has been true for at least PT
    Duration et() const { return et_; }   // elapsed time, never above PT

private:
    bool running_ = false; // IN was true on the previous update
    TimePoint start_{};    // when IN last went from false to true
    bool q_ = false;
    Duration et_{};
};

} // namespace softplc
