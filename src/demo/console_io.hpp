#pragma once
#include "core/io_driver.hpp"
#include "core/plc_time.hpp"
#include <cstdint>
#include <vector>

namespace softplc {

// An I/O driver with no real hardware: inputs stay at 0, and every output bit
// change is printed to the terminal with a timestamp.
class ConsoleIo : public IoDriver {
public:
    explicit ConsoleIo(TimePoint startTime);

    void readInputs(ProcessImage& image) override;
    void writeOutputs(const ProcessImage& image) override;

private:
    TimePoint startTime_;
    std::vector<std::uint8_t> lastOutputs_;
};

} // namespace softplc
