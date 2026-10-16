#pragma once
#include "core/plc_time.hpp"
#include "core/process_image.hpp"
#include <cstdint>

namespace softplc {

// What the scan cycle tells the program about the current cycle.
struct CycleContext {
    TimePoint now;              // the same for the whole cycle, like a PLC's cycle timestamp
    std::uint64_t cycleNumber;  // 0 for the first cycle
};

// The user program the PLC executes once per scan cycle. For now programs
// are written in C++; later the Structured Text interpreter implements this.
class Program {
public:
    virtual ~Program() = default;
    virtual void execute(ProcessImage& image, const CycleContext& context) = 0;
};

} // namespace softplc
