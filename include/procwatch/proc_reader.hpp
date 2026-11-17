#pragma once

#include "procwatch/types.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <sys/types.h>

namespace procwatch
{

// Describes the outcome of a process metrics read
enum class ReadStatus
{
    Ok,
    ProcessGone,
    ReadError,
    ParseError
};

// Contains the result of a process metrics read
struct ReadResult
{
    ReadStatus status{ReadStatus::Ok};
    ProcessMetrics metrics;
    std::string error;
};

// The fields from /proc/<pid>/stat that procwatch needs
struct StatFields
{
    char state{};
    std::uint64_t utime_ticks{};
    std::uint64_t stime_ticks{};
};

// Parses the content of /proc/<pid>/stat
// Returns std::nullopt if the content is invalid
std::optional<StatFields> parse_stat(std::string_view content);

// Reads VmRSS from /proc/<pid>/status and converts it from kB to bytes
// Returns std::nullopt if the value is missing or invalid.
std::optional<std::uint64_t> parse_rss_bytes(std::string_view content);

// Converts CPU clock ticks to seconds.
// Throws std::invalid_argument if ticks_per_second is not positive
double ticks_to_seconds(std::uint64_t ticks, long ticks_per_second);

// Returns the number of clock ticks per second reported by Linux.
// Throws std::runtime_error if the value cannot be determined
long system_ticks_per_second();

// Reads RSS and CPU time for a single process from the /proc filesystem
class ProcReader
{
public:
    // proc_root and ticks_per_second can be set explicitly to make testing
    // possible without relying on the real /proc filesystem or system tick rate
    explicit ProcReader(
        std::filesystem::path proc_root = "/proc",
        long ticks_per_second = system_ticks_per_second()
    );

    // Reads the current RSS and CPU time for the given process
    // Read errors are returned through ReadResult rather than exceptions
    ReadResult read(pid_t pid) const;

private:
    std::filesystem::path proc_root_;
    long ticks_per_second_;
};

}
