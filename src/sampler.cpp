#include "procwatch/sampler.hpp"

#include <stdexcept>
#include <utility>

namespace procwatch
{

// Creates a sampler for one process with a fixed sampling interval
Sampler::Sampler(
    pid_t pid,
    ProcReader reader,
    std::chrono::milliseconds interval
)
    : pid_(pid),
      reader_(std::move(reader)),
      interval_(interval)
{
    // A zero or negative interval would make every sample due immediately
    if (interval_ <= std::chrono::milliseconds::zero())
    {
        throw std::invalid_argument(
            "Sampling interval must be greater than zero"
        );
    }
}

// Reads the process metrics once and stores them if the read succeeded
ReadResult Sampler::sample(Clock::time_point now)
{
    // The schedule is updated before reading, so a failed read still
    // uses up its slot
    if (next_sample_time_ == Clock::time_point{})
    {
        // First call: the schedule starts one interval after this sample
        next_sample_time_ = now + interval_;
    }
    else
    {
        // Advance from the scheduled time rather than from now, so small
        // delays do not accumulate
        next_sample_time_ += interval_;

        // If the caller is late by a full interval or more, skip the missed
        // slots instead of taking several samples in quick succession
        if (next_sample_time_ <= now)
        {
            next_sample_time_ = now + interval_;
        }
    }

    ReadResult result = reader_.read(pid_);

    if (result.status == ReadStatus::Ok)
    {
        samples_.push_back(Sample{now, result.metrics});
    }

    return result;
}

// Returns the time at which the next sample should be taken
Sampler::Clock::time_point Sampler::next_sample_time() const
{
    return next_sample_time_;
}

// Returns all successful samples collected so far
const std::vector<Sample>& Sampler::samples() const
{
    return samples_;
}

}