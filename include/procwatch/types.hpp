#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace procwatch
{

// Exit codes returned by procwatch itself
enum class ExitCode
{
    Success = 0,
    StabilityIssue = 1,
    UsageError = 2,
    InternalError = 3
};

// Raw resource usage metrics collected from the Linux process
struct ProcessMetrics
{
    std::uint64_t rss_bytes{};
    std::uint64_t cpu_time_ticks{};
    double cpu_time_seconds{};
};

// A single resource usage measurement with its timestamp
struct Sample
{
    std::chrono::steady_clock::time_point timestamp;
    ProcessMetrics metrics;
};

// A stability issue detected during process monitoring
struct Finding
{
    std::string type;
    std::string description;
    std::chrono::steady_clock::time_point timestamp;
};

// Describes how the monitored process terminated
struct ProcessResult
{
    std::optional<int> exit_code;
    int termination_signal{0};
    bool exited_normally{false};
};

// Contains the complete results of a monitoring session
struct ReportData
{
    std::vector<Sample> samples;
    std::vector<Finding> findings;
    ProcessResult process_result;
    bool stable{true};
};

} // namespace procwatch
