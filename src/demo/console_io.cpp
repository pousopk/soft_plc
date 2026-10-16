#include "demo/console_io.hpp"
#include <chrono>
#include <fmt/format.h>

namespace softplc {

ConsoleIo::ConsoleIo(TimePoint startTime) : startTime_(startTime) {}

void ConsoleIo::readInputs(ProcessImage& /*image*/) {}

void ConsoleIo::writeOutputs(const ProcessImage& image) {
    const auto outputs = image.bytes(Area::Output);
    if (lastOutputs_.size() != outputs.size()) {
        lastOutputs_.assign(outputs.size(), 0);
    }

    const std::chrono::duration<double> sinceStart = Clock::now() - startTime_;
    for (std::size_t byte = 0; byte < outputs.size(); ++byte) {
        const auto changed = static_cast<std::uint8_t>(outputs[byte] ^ lastOutputs_[byte]);
        for (unsigned bit = 0; bit < 8; ++bit) {
            if (((changed >> bit) & 1) != 0) {
                const Address address{Area::Output, AccessSize::Bit, byte, bit};
                fmt::print("[{:8.3f} s] {} = {}\n", sinceStart.count(), address.toString(), (outputs[byte] >> bit) & 1);
            }
        }
        lastOutputs_[byte] = outputs[byte];
    }
}

} // namespace softplc
