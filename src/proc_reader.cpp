#include "procwatch/proc_reader.hpp"

#include <cerrno>
#include <charconv>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

namespace procwatch
{

// Internal helpers used only by this source file
namespace
{

// Field positions in /proc/<pid>/stat after the process state
constexpr std::size_t utime_index = 11;
constexpr std::size_t stime_index = 12;

// Linux reports VmRSS in kilobytes.
constexpr std::uint64_t bytes_per_kb = 1024;

// Stores file content or the error returned by the system
struct FileContent
{
    int error{0};
    std::string data;
};

// Reads a file and returns either its content or the system error
FileContent read_file(const std::filesystem::path& path)
{
    // Open the file in read-only mode and prevent descriptor inheritance
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);

    if (fd == -1)
    {
        return {errno, {}};
    }

    std::string content;
    char buffer[4096];

    // Read the file in chunks until the end is reached
    while (true)
    {
        const ssize_t bytes_read = ::read(fd, buffer, sizeof(buffer));

        if (bytes_read > 0)
        {
            content.append(buffer, static_cast<std::size_t>(bytes_read));
            continue;
        }

        if (bytes_read == 0)
        {
            break;
        }

        // Retry the read if it was interrupted by a signal
        if (errno == EINTR)
        {
            continue;
        }

        const int error = errno;
        ::close(fd);
        return {error, {}};
    }

    // Close the file descriptor after reading is complete
    if (::close(fd) == -1)
    {
        return {errno, {}};
    }

    return {0, std::move(content)};
}

// Checks whether the process no longer exists
bool is_process_gone(int error)
{
    return error == ENOENT || error == ESRCH;
}

// Removes leading spaces, tabs and newline characters
std::string_view skip_spaces(std::string_view text)
{
    const std::size_t first = text.find_first_not_of(" \t\n");

    if (first == std::string_view::npos)
    {
        return {};
    }

    return text.substr(first);
}

// Parses an unsigned integer without creating a temporary string
bool parse_unsigned(std::string_view text, std::uint64_t& value)
{
    if (text.empty())
    {
        return false;
    }

    const char* begin = text.data();
    const char* end = begin + text.size();

    const auto result = std::from_chars(begin, end, value);

    return result.ec == std::errc{} && result.ptr == end;
}

// Extracts the next token and advances the remaining input
bool next_token(std::string_view& text, std::string_view& token)
{
    text = skip_spaces(text);

    if (text.empty())
    {
        return false;
    }

    const std::size_t end = text.find_first_of(" \t\n");

    if (end == std::string_view::npos)
    {
        token = text;
        text = {};
    }
    else
    {
        token = text.substr(0, end);
        text.remove_prefix(end);
    }

    return true;
}

// Creates a failed read result with a status and error message
ReadResult failure(ReadStatus status, std::string message)
{
    ReadResult result;
    result.status = status;
    result.error = std::move(message);
    return result;
}

// Converts a file access error into the corresponding read status
ReadResult file_failure(const FileContent& file, const std::string& path)
{
    if (is_process_gone(file.error))
    {
        return failure(
            ReadStatus::ProcessGone,
            "Process disappeared while reading " + path
        );
    }

    return failure(
        ReadStatus::ReadError,
        "Failed to read " + path + ": " +
            std::error_code(file.error, std::generic_category()).message()
    );
}

} // namespace

// Parses the process state and CPU time fields from /proc/<pid>/stat
std::optional<StatFields> parse_stat(std::string_view content)
{
    // The command name is enclosed in parentheses and may itself contain spaces.
    const std::size_t closing_parenthesis = content.rfind(')');

    if (closing_parenthesis == std::string_view::npos)
    {
        return std::nullopt;
    }

    // Start parsing immediately after the process name
    std::string_view fields = content.substr(closing_parenthesis + 1);
    std::string_view token;

    StatFields result;

    // Read fields up to stime, extracting only the values we need
    for (std::size_t index = 0; index <= stime_index; ++index)
    {
        if (!next_token(fields, token))
        {
            return std::nullopt;
        }

        // The first field after the process name is the process state
        if (index == 0)
        {
            if (token.size() != 1)
            {
                return std::nullopt;
            }

            result.state = token.front();
        }
        // Extract the user CPU time.
        else if (index == utime_index)
        {
            if (!parse_unsigned(token, result.utime_ticks))
            {
                return std::nullopt;
            }
        }
        // Extract the system CPU time.
        else if (index == stime_index)
        {
            if (!parse_unsigned(token, result.stime_ticks))
            {
                return std::nullopt;
            }
        }
    }

    return result;
}

// Reads VmRSS from /proc/<pid>/status and converts it to bytes
std::optional<std::uint64_t> parse_rss_bytes(std::string_view content)
{
    constexpr std::string_view rss_key = "VmRSS:";

    // Search the status file for the VmRSS entry.
    while (!content.empty())
    {
        const std::size_t line_end = content.find('\n');

        const std::string_view line =
            content.substr(0, line_end == std::string_view::npos
                                  ? content.size()
                                  : line_end);

        if (line.substr(0, rss_key.size()) == rss_key)
        {
            // Extract the memory value and its unit
            std::string_view value_text = line.substr(rss_key.size());
            std::string_view value_token;
            std::string_view unit_token;
            std::uint64_t rss_kb = 0;

            // Reject invalid values and units other than kB
            if (!next_token(value_text, value_token) ||
                !parse_unsigned(value_token, rss_kb) ||
                !next_token(value_text, unit_token) ||
                unit_token != "kB")
            {
                return std::nullopt;
            }

            // Prevent overflow when converting kilobytes to bytes
            if (rss_kb > std::numeric_limits<std::uint64_t>::max() /
                             bytes_per_kb)
            {
                return std::nullopt;
            }

            return rss_kb * bytes_per_kb;
        }

        if (line_end == std::string_view::npos)
        {
            break;
        }

        // Continue with the next line
        content.remove_prefix(line_end + 1);
    }

    return std::nullopt;
}

// Converts CPU clock ticks into seconds using the system tick rate
double ticks_to_seconds(std::uint64_t ticks, long ticks_per_second)
{
    if (ticks_per_second <= 0)
    {
        throw std::invalid_argument(
            "ticks_per_second must be greater than zero"
        );
    }

    return static_cast<double>(ticks) /
           static_cast<double>(ticks_per_second);
}

// Returns the clock tick rate provided by the Linux system
long system_ticks_per_second()
{
    const long ticks_per_second = ::sysconf(_SC_CLK_TCK);

    if (ticks_per_second <= 0)
    {
        throw std::runtime_error(
            "Could not determine the system clock tick rate"
        );
    }

    return ticks_per_second;
}

// Initializes the reader with a proc filesystem root and clock tick rate
ProcReader::ProcReader(
    std::filesystem::path proc_root,
    long ticks_per_second
)
    : proc_root_(std::move(proc_root)),
      ticks_per_second_(ticks_per_second)
{
    // Reject invalid tick rates before reading process metrics
    if (ticks_per_second_ <= 0)
    {
        throw std::invalid_argument(
            "ticks_per_second must be greater than zero"
        );
    }
}

// Reads memory and CPU metrics for the process with the given PID
ReadResult ProcReader::read(pid_t pid) const
{
    // A valid Linux process ID must be greater than zero
    if (pid <= 0)
    {
        return failure(
            ReadStatus::ReadError,
            "PID must be greater than zero"
        );
    }

    // Build the paths to the process-specific proc files
    const std::filesystem::path process_dir =
        proc_root_ / std::to_string(pid);

    const std::string stat_path = (process_dir / "stat").string();
    const FileContent stat_file = read_file(stat_path);

    if (stat_file.error != 0)
    {
        return file_failure(stat_file, stat_path);
    }

    // Parse the process state and CPU time values
    const auto stat_fields = parse_stat(stat_file.data);

    if (!stat_fields)
    {
        return failure(
            ReadStatus::ParseError,
            "Could not parse " + stat_path
        );
    }

    // Zombie and dead processes are no longer available for monitoring
    if (stat_fields->state == 'Z' || stat_fields->state == 'X')
    {
        return failure(
            ReadStatus::ProcessGone,
            "Process has already terminated"
        );
    }

    const std::string status_path = (process_dir / "status").string();
    const FileContent status_file = read_file(status_path);

    if (status_file.error != 0)
    {
        return file_failure(status_file, status_path);
    }

    // Extract the resident memory size from the status file
    const auto rss_bytes = parse_rss_bytes(status_file.data);

    if (!rss_bytes)
    {
        return failure(
            ReadStatus::ParseError,
            "Could not parse VmRSS from " + status_path
        );
    }

    // Check that adding user and system CPU ticks cannot overflow
    if (stat_fields->utime_ticks >
        std::numeric_limits<std::uint64_t>::max() -
            stat_fields->stime_ticks)
    {
        return failure(
            ReadStatus::ParseError,
            "CPU time value is out of range"
        );
    }

    // Calculate the total CPU time in clock ticks
    const std::uint64_t cpu_ticks =
        stat_fields->utime_ticks + stat_fields->stime_ticks;

    // Build the result containing the collected process metrics
    ReadResult result;
    result.status = ReadStatus::Ok;
    result.metrics.rss_bytes = *rss_bytes;
    result.metrics.cpu_time_ticks = cpu_ticks;
    result.metrics.cpu_time_seconds =
        ticks_to_seconds(cpu_ticks, ticks_per_second_);

    return result;
}

}
