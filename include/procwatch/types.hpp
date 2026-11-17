#pragma once

#include <chrono>
#include <cstdint>
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

// How the monitored process ended
enum class Termination
{
    // The process exited on its own; exit_code is set
    Exited,
    // The process was killed by a signal that procwatch did not send
    Signaled,
    // procwatch ended the process after a timeout or SIGINT/SIGTERM
    StoppedByProcwatch
};

// Describes how the monitored process terminated
struct ProcessResult
{
    Termination termination{Termination::Exited};

    // Exit code of the monitored process for Termination::Exited, else 0
    int exit_code{0};

    // Signal number for Termination::Signaled, else 0
    int signal{0};
};

// Contains the complete results of a monitoring session
struct ReportData
{
    // Reference point for all steady_clock timestamps in the report
    std::chrono::steady_clock::time_point start_time;

    // Wall-clock time at which monitoring started
    std::chrono::system_clock::time_point started_at;

    // Time at which monitoring ended
    std::chrono::steady_clock::time_point end_time;

    std::vector<Sample> samples;
    std::vector<Finding> findings;
    ProcessResult process_result;
    bool stable{true};
};

} // namespace procwatch