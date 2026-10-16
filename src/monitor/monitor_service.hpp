#pragma once
#include "core/process_image.hpp"
#include "core/scan_cycle.hpp"
#include "st/st_program.hpp"
#include <string>
#include <string_view>
#include <vector>

namespace softplc::monitor {

// Answers monitoring requests: one line of text in, one line of text out.
// Every answer starts with "OK" or "ERR".
//
//   LIST                     OK count:INT lamp:BOOL t1.Q:BOOL t1.ET:TIME ...
//   READ count t1.ET         OK 42 T#1s200ms        (? for an unknown name)
//   WRITE count 100          OK                     (once; the program may overwrite it)
//   FORCE lamp TRUE          OK                     (held every cycle until UNFORCE)
//   UNFORCE lamp             OK
//   FORCES                   OK lamp=TRUE
//   STATUS                   OK RUN cycles=1234 cycle_ms=10 avg_us=12.5 max_us=80.1 overruns=0
//                            OK STOP cycles=... error=line 3, column 8: division by zero
//
// It must only be called between cycles (from ScanCycle's idle task), so it
// always sees a consistent state and never races with the program.
class MonitorService {
public:
    MonitorService(st::StProgram& program, ProcessImage& image, const ScanCycle& cycle);

    std::string handle(std::string_view request);

private:
    std::string list() const;
    std::string read(const std::vector<std::string>& paths) const;
    std::string write(const std::vector<std::string>& args);
    std::string force(const std::vector<std::string>& args);
    std::string unforce(const std::vector<std::string>& args);
    std::string forces() const;
    std::string status() const;

    st::StProgram& program_;
    ProcessImage& image_;
    const ScanCycle& cycle_;
};

} // namespace softplc::monitor
