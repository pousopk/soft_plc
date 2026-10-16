#pragma once
#include "core/plc_time.hpp"

namespace softplc {

// The three IEC 61131-3 timers. Call update() once per scan cycle, like
// calling a timer instance in a PLC program. Each object is one timer
// instance and remembers its own state between cycles.

// TON, on-delay: Q switches on once IN has been on for PT.
//
//   IN  ___/‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾\______
//   Q   _________/‾‾‾‾‾‾‾‾‾\______
//          |<-- PT -->|
class Ton {
public:
    void update(bool in, Duration preset, TimePoint now);

    bool q() const { return q_; }       // output: IN has been true for at least PT
    Duration et() const { return et_; } // elapsed time, never above PT

private:
    bool running_ = false; // IN was true on the previous update
    TimePoint start_{};    // when IN last went from false to true
    bool q_ = false;
    Duration et_{};
};

// TOF, off-delay: Q follows IN on, but stays on for PT after IN goes off.
//
//   IN  ___/‾‾‾‾‾‾‾\______________
//   Q   ___/‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾\____
//                  |<-- PT -->|
class Tof {
public:
    void update(bool in, Duration preset, TimePoint now);

    bool q() const { return q_; }
    Duration et() const { return et_; }

private:
    bool timing_ = false; // IN went off and the delay is running
    TimePoint start_{};
    bool q_ = false;
    Duration et_{};
};

// TP, pulse: a rising edge on IN gives a pulse of exactly PT on Q, however
// long or short IN stays on. Edges during the pulse are ignored.
//
//   IN  ___/‾\_______/‾‾‾‾‾‾‾‾‾‾‾‾‾‾\__
//   Q   ___/‾‾‾‾‾‾\__/‾‾‾‾‾‾\__________
//          |<-PT->|  |<-PT->|
class Tp {
public:
    void update(bool in, Duration preset, TimePoint now);

    bool q() const { return q_; }
    Duration et() const { return et_; }

private:
    bool previousIn_ = false;
    bool pulsing_ = false; // a pulse is running
    TimePoint start_{};
    bool q_ = false;
    Duration et_{};
};

} // namespace softplc
