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
        applyForces(image);
        interpreter_.runCycle(image, context.now);
        applyForces(image);
    } catch (const RuntimeError& error) {
        stopError_ = error;
        const auto outputs = image.bytes(Area::Output);
        std::fill(outputs.begin(), outputs.end(), std::uint8_t{0});
        if (onStop_) {
            onStop_(error);
        }
    }
}

bool StProgram::force(std::string_view path, const Value& value) {
    const auto variable = interpreter_.describe(path);
    if (!variable) {
        return false;
    }
    forces_[variable->path] = value;
    return true;
}

bool StProgram::unforce(std::string_view path) {
    const auto variable = interpreter_.describe(path);
    return variable && forces_.erase(variable->path) > 0;
}

void StProgram::applyForces(ProcessImage& image) {
    for (const auto& [path, value] : forces_) {
        interpreter_.setValue(path, value, image);
    }
}

} // namespace softplc::st
