#pragma once

#include "procwatch/config.hpp"
#include "procwatch/types.hpp"

#include <chrono>
#include <optional>
#include <stdexcept>
#include <vector>

namespace procwatch
{

// Finding types produced by the Analyzer
inline constexpr const char* memory_growth_finding = "memory_growth";
inline constexpr const char* sustained_cpu_finding = "sustained_cpu";

// Evaluates the samples of one monitoring session against the configured
// thresholds. The Analyzer keeps no state between calls, so the same
// samples always produce the same findings
class Analyzer
{
public:
    // Copies the thresholds and durations it needs from config
    // Throws std::invalid_argument if a threshold is negative, NaN or
    // infinite, or if a duration is negative
    explicit Analyzer(const Config& config);

    // Returns all findings for the given samples, ordered by timestamp
    //
    // Sample timestamps must be strictly increasing. Throws
    // std::invalid_argument if two samples have the same timestamp or are
    // out of order. Fewer than two samples are valid and produce no findings
    //
    // Memory growth: samples within the warm-up period, which starts at the
    // first sample, are ignored. The first sample at or after the end of the
    // warm-up with a non-zero RSS becomes the baseline. The baseline stays
    // fixed for the entire analysis
    //
    // CPU usage is evaluated only after the warm-up period
    // An interval crossing the warm-up boundary is counted proportionally
    std::vector<Finding> analyze(const std::vector<Sample>& samples) const;

private:
    std::optional<Finding> find_memory_growth(
        const std::vector<Sample>& samples
    ) const;

    std::vector<Finding> find_sustained_cpu(
        const std::vector<Sample>& samples
    ) const;

    std::chrono::milliseconds warmup_duration_;
    std::chrono::milliseconds cpu_sustained_duration_;
    double memory_growth_threshold_percent_;
    double cpu_usage_threshold_percent_;
};

}