#pragma once

#include "procwatch/proc_reader.hpp"
#include "procwatch/types.hpp"

#include <chrono>
#include <vector>

#include <sys/types.h>

namespace procwatch
{

// Collects timestamped metrics of one process at a fixed interval
//
// The Sampler does not sleep and does not read the clock on its own. The
// caller (the Supervisor) asks when the next sample is due, waits until then,
// and calls sample(). This keeps the Supervisor in control of waiting, so it
// can also react to process termination and signals, and it lets unit tests
// pass exact time points instead of relying on real time
class Sampler
{
public:
    using Clock = std::chrono::steady_clock;

    // Throws std::invalid_argument if interval is not positive
    Sampler(
        pid_t pid,
        ProcReader reader,
        std::chrono::milliseconds interval
    );

    // Reads the current metrics of the process and stores them as a Sample
    // with the given timestamp. Failed reads are returned to the caller and
    // are not stored. Each call advances the schedule, whether or not the
    // read succeeds
    ReadResult sample(Clock::time_point now = Clock::now());

    // Time at which the next sample is due
    // Before the first sample this is a time point in the past, so the first
    // sample is due immediately
    Clock::time_point next_sample_time() const;

    // All successful samples in the order in which they were taken
    const std::vector<Sample>& samples() const;

private:
    pid_t pid_;
    ProcReader reader_;
    std::chrono::milliseconds interval_;
    Clock::time_point next_sample_time_{};
    std::vector<Sample> samples_;
};

}