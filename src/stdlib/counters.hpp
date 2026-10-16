#pragma once
#include <cstdint>

namespace softplc {

// IEC 61131-3 counters. Both count on the *rising edge* of their count input,
// not while it's on, so a sensor that stays on for many cycles counts once.

// CTU, up counter: CV goes up by one on each rising edge of CU; R resets it
// to 0. Q is on once CV has reached PV.
class Ctu {
public:
    void update(bool cu, bool reset, std::int16_t preset);

    bool q() const { return q_; }
    std::int16_t cv() const { return cv_; }

private:
    bool previousCu_ = false;
    bool q_ = false;
    std::int16_t cv_ = 0;
};

// CTD, down counter: CV goes down by one on each rising edge of CD; LD loads
// PV into CV. Q is on once CV has reached 0 (or below).
class Ctd {
public:
    void update(bool cd, bool load, std::int16_t preset);

    bool q() const { return q_; }
    std::int16_t cv() const { return cv_; }

private:
    bool previousCd_ = false;
    bool q_ = false;
    std::int16_t cv_ = 0;
};

} // namespace softplc
