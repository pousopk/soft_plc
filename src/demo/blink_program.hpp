#pragma once
#include "core/program.hpp"
#include "stdlib/timers.hpp"

namespace softplc {

// Demo program, the classic two-timer blinker. In Structured Text it would be:
//
//   onTimer(IN := NOT offTimer.Q, PT := T#500ms);
//   offTimer(IN := onTimer.Q, PT := T#500ms);
//   %QX0.0 := onTimer.Q;
//   %MD0 := cycle counter;
class BlinkProgram : public Program {
public:
    void execute(ProcessImage& image, const CycleContext& context) override;

private:
    Ton onTimer_;
    Ton offTimer_;
};

} // namespace softplc
