#pragma once

#include "procwatch/config.hpp"
#include "procwatch/types.hpp"

#include <stdexcept>

namespace procwatch
{

// Finding type for a target that failed on its own
inline constexpr const char* abnormal_termination_finding =
    "abnormal_termination";

// The target command could not be executed
class LaunchError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

// Launches the target as a direct child process and monitors it until it
// ends, the maximum duration is reached, or procwatch receives SIGINT or
// SIGTERM.
//
// The maximum duration is measured from the first sample. On a stop request
// the target receives SIGTERM, followed by SIGKILL if it is still running
// after a short grace period.
//
// The target stays in the process group of procwatch, so Ctrl+C reaches both.
// How the target ended is decided from what procwatch has observed when it
// collects the target:
// - With an observed stop request, an exit or the end by SIGINT, SIGTERM or
//   SIGKILL counts as stopped by procwatch. Any other signal is abnormal.
// - Without an observed stop request, any signal is abnormal, even if it came
//   from the same Ctrl+C.
class Supervisor
{
public:
    explicit Supervisor(const Config& config);

    // Runs one monitoring session and returns its complete results.
    // SIGINT, SIGTERM and SIGCHLD are managed during the call and restored
    // afterwards. The target is never left running when this function throws,
    // unless the system refuses to terminate it.
    //
    // Throws LaunchError if the command cannot be executed and
    // std::runtime_error for other failures
    ReportData run();

private:
    Config config_;
};

}