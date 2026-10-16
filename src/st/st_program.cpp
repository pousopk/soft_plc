#include "st/st_program.hpp"
#include <algorithm>

namespace softplc::st {

StProgram::StProgram(CompilationUnit unit, StopHandler onStop, std::uint64_t loopLimit)
    : interpreter_(std::move(unit), loopLimit), onStop_(std::move(onStop)) {}

void StProgram::execute(ProcessImage& image, const CycleContext& context) {
    if (stopped()) {
        return;
    }
    try {
        interpreter_.runCycle(image, context.now);
    } catch (const RuntimeError& error) {
        stopError_ = error;
        const auto outputs = image.bytes(Area::Output);
        std::fill(outputs.begin(), outputs.end(), std::uint8_t{0});
        if (onStop_) {
            onStop_(error);
        }
    }
}

} // namespace softplc::st
