#pragma once
#include "core/process_image.hpp"

namespace softplc {

// Connects the process image to the outside world: real hardware, a
// simulator such as factory_sim, or just the console.
class IoDriver {
public:
    virtual ~IoDriver() = default;

    // Called at the start of every cycle: copy physical inputs into the %I area.
    virtual void readInputs(ProcessImage& image) = 0;

    // Called at the end of every cycle: copy the %Q area to physical outputs.
    virtual void writeOutputs(const ProcessImage& image) = 0;
};

} // namespace softplc
