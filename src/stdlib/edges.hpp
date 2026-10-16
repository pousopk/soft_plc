#pragma once

namespace softplc {

// Edge detection: Q is on for exactly one call when CLK changes.
//
//   CLK     ___/‾‾‾‾‾‾‾‾\_____
//   R_TRIG  ___/\_____________   one cycle, at the rising edge
//   F_TRIG  ____________/\____   one cycle, at the falling edge

class RTrig {
public:
    void update(bool clk) {
        q_ = clk && !previous_;
        previous_ = clk;
    }
    bool q() const { return q_; }

private:
    bool previous_ = false;
    bool q_ = false;
};

// Note: the textbook IEC definition of F_TRIG gives a pulse on the very first
// call if CLK starts off. This one only reacts to a real on -> off change.
class FTrig {
public:
    void update(bool clk) {
        q_ = !clk && previous_;
        previous_ = clk;
    }
    bool q() const { return q_; }

private:
    bool previous_ = false;
    bool q_ = false;
};

// Flip-flops: S sets Q1 on, R resets it off, and Q1 remembers its state when
// both are off. They differ only when S and R are on at the same time.

// SR, set-dominant: if both are on, Q1 is on.
class Sr {
public:
    void update(bool set, bool reset) { q1_ = set || (!reset && q1_); }
    bool q1() const { return q1_; }

private:
    bool q1_ = false;
};

// RS, reset-dominant: if both are on, Q1 is off. The usual choice for
// starting a motor, so that "stop" always wins.
class Rs {
public:
    void update(bool set, bool reset) { q1_ = !reset && (set || q1_); }
    bool q1() const { return q1_; }

private:
    bool q1_ = false;
};

} // namespace softplc
