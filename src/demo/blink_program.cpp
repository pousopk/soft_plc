#include "demo/blink_program.hpp"
#include <chrono>

namespace softplc {

using namespace std::chrono_literals;

void BlinkProgram::execute(ProcessImage& image, const CycleContext& context) {
    onTimer_.update(!offTimer_.q(), 500ms, context.now);
    offTimer_.update(onTimer_.q(), 500ms, context.now);

    image.writeBit(Area::Output, 0, 0, onTimer_.q());
    image.writeDWord(Area::Marker, 0, static_cast<std::uint32_t>(context.cycleNumber));
}

} // namespace softplc
