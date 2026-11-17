#include "procwatch/report.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstddef>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

namespace procwatch
{

// Internal helpers used only by this source file
namespace
{

using SteadyTime = std::chrono::steady_clock::time_point;
using Seconds = std::chrono::duration<double>;

// Exit statuses reported by waitpid() fit into 8 bits
constexpr int max_exit_code = 255;

// Attempts to find an unused temporary file name before giving up
constexpr int max_temp_file_attempts = 100;

// Returns the time since the start of monitoring in seconds
double offset_seconds(SteadyTime time, SteadyTime start)
{
    return Seconds(time - start).count();
}

// Formats a wall-clock time as ISO 8601 in UTC with milliseconds
std::string format_utc(std::chrono::system_clock::time_point time)
{
    const auto whole_seconds = std::chrono::floor<std::chrono::seconds>(time);
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            time - whole_seconds);

    const std::time_t time_value =
        std::chrono::system_clock::to_time_t(whole_seconds);

    std::tm utc{};
    if (::gmtime_r(&time_value, &utc) == nullptr)
    {
        throw std::invalid_argument("Start time cannot be represented");
    }

    std::ostringstream stream;
    stream << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.'
           << std::setw(3) << std::setfill('0') << milliseconds.count()
           << 'Z';
    return stream.str();
}

// Formats a value with one decimal place
std::string format_number(double value)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(1) << value;
    return stream.str();
}

// Returns the system error message for an errno value
std::string error_message(int error)
{
    return std::error_code(error, std::generic_category()).message();
}

// Returns the report name of a termination kind
const char* termination_name(Termination termination)
{
    switch (termination)
    {
        case Termination::Exited:
            return "exited";
        case Termination::Signaled:
            return "signaled";
        case Termination::StoppedByProcwatch:
            return "stopped_by_procwatch";
    }

    throw std::invalid_argument("Unknown process termination kind");
}

// Checks that only the fields belonging to the termination kind are set.
// Fields that do not apply must be zero
void validate_process_result(const ProcessResult& result)
{
    switch (result.termination)
    {
        case Termination::Exited:
            if (result.exit_code < 0 || result.exit_code > max_exit_code)
            {
                throw std::invalid_argument(
                    "Exit code must be between 0 and 255"
                );
            }
            if (result.signal != 0)
            {
                throw std::invalid_argument(
                    "An exited process must not have a signal"
                );
            }
            return;

        case Termination::Signaled:
            if (result.signal <= 0 || result.signal >= NSIG)
            {
                throw std::invalid_argument("Signal number is not valid");
            }
            if (result.exit_code != 0)
            {
                throw std::invalid_argument(
                    "A signaled process must not have an exit code"
                );
            }
            return;

        case Termination::StoppedByProcwatch:
            if (result.exit_code != 0 || result.signal != 0)
            {
                throw std::invalid_argument(
                    "A process stopped by procwatch must not have an exit "
                    "code or signal"
                );
            }
            return;
    }

    throw std::invalid_argument("Unknown process termination kind");
}

// Checks timestamps and cumulative CPU time of the samples
void validate_samples(const ReportData& data)
{
    for (std::size_t i = 0; i < data.samples.size(); ++i)
    {
        const Sample& sample = data.samples[i];

        if (sample.timestamp < data.start_time ||
            sample.timestamp > data.end_time)
        {
            throw std::invalid_argument(
                "Sample lies outside the monitoring period"
            );
        }

        if (!std::isfinite(sample.metrics.cpu_time_seconds) ||
            sample.metrics.cpu_time_seconds < 0.0)
        {
            throw std::invalid_argument(
                "Sample CPU time must be finite and non-negative"
            );
        }

        if (i == 0)
        {
            continue;
        }

        const Sample& previous = data.samples[i - 1];

        if (sample.timestamp <= previous.timestamp)
        {
            throw std::invalid_argument(
                "Sample timestamps must be strictly increasing"
            );
        }

        if (sample.metrics.cpu_time_ticks < previous.metrics.cpu_time_ticks ||
            sample.metrics.cpu_time_seconds < previous.metrics.cpu_time_seconds)
        {
            throw std::invalid_argument("Sample CPU time must not decrease");
        }
    }
}

// Checks that findings lie within the session in chronological order.
// Two findings may share a timestamp
void validate_findings(const ReportData& data)
{
    for (std::size_t i = 0; i < data.findings.size(); ++i)
    {
        const Finding& finding = data.findings[i];

        if (finding.timestamp < data.start_time ||
            finding.timestamp > data.end_time)
        {
            throw std::invalid_argument(
                "Finding lies outside the monitoring period"
            );
        }

        if (i > 0 && finding.timestamp < data.findings[i - 1].timestamp)
        {
            throw std::invalid_argument(
                "Findings must be in chronological order"
            );
        }
    }
}

// Rejects report data that cannot describe a real monitoring session
void validate_report_data(const ReportData& data)
{
    if (data.end_time < data.start_time)
    {
        throw std::invalid_argument("Monitoring ends before it starts");
    }

    validate_process_result(data.process_result);
    validate_samples(data);
    validate_findings(data);

    // The verdict must match the findings
    if (data.stable != data.findings.empty())
    {
        throw std::invalid_argument(
            "Stability verdict does not match the findings"
        );
    }
}

nlohmann::json config_to_json(const Config& config)
{
    return {
        {"command", config.command},
        {"sampling_interval_ms", config.sampling_interval.count()},
        {"max_duration_ms", config.max_duration.count()},
        {"warmup_duration_ms", config.warmup_duration.count()},
        {"cpu_sustained_duration_ms", config.cpu_sustained_duration.count()},
        {"memory_growth_threshold_percent",
         config.memory_growth_threshold_percent},
        {"cpu_usage_threshold_percent", config.cpu_usage_threshold_percent}
    };
}

// Fields that do not apply to the termination kind are null
nlohmann::json process_to_json(const ProcessResult& result)
{
    nlohmann::json process = {
        {"termination", termination_name(result.termination)},
        {"exit_code", nullptr},
        {"signal", nullptr}
    };

    if (result.termination == Termination::Exited)
    {
        process["exit_code"] = result.exit_code;
    }
    else if (result.termination == Termination::Signaled)
    {
        process["signal"] = result.signal;
    }

    return process;
}

// Summarizes the samples instead of listing each one
nlohmann::json samples_to_json(const ReportData& data)
{
    const auto& samples = data.samples;

    nlohmann::json summary = {
        {"count", samples.size()},
        {"first_offset_seconds", nullptr},
        {"last_offset_seconds", nullptr},
        {"rss_bytes", nullptr},
        {"cpu_time_seconds", nullptr}
    };

    if (samples.empty())
    {
        return summary;
    }

    const auto peak = std::max_element(
        samples.begin(),
        samples.end(),
        [](const Sample& left, const Sample& right)
        {
            return left.metrics.rss_bytes < right.metrics.rss_bytes;
        }
    );

    summary["first_offset_seconds"] =
        offset_seconds(samples.front().timestamp, data.start_time);
    summary["last_offset_seconds"] =
        offset_seconds(samples.back().timestamp, data.start_time);
    summary["rss_bytes"] = {
        {"first", samples.front().metrics.rss_bytes},
        {"last", samples.back().metrics.rss_bytes},
        {"peak", peak->metrics.rss_bytes}
    };
    summary["cpu_time_seconds"] = samples.back().metrics.cpu_time_seconds;

    return summary;
}

nlohmann::json findings_to_json(const ReportData& data)
{
    nlohmann::json findings = nlohmann::json::array();

    for (const Finding& finding : data.findings)
    {
        findings.push_back({
            {"type", finding.type},
            {"offset_seconds",
             offset_seconds(finding.timestamp, data.start_time)},
            {"description", finding.description}
        });
    }

    return findings;
}

// Describes how the process ended in one short phrase
std::string describe_termination(const ProcessResult& result)
{
    switch (result.termination)
    {
        case Termination::Exited:
            return "exited with code " + std::to_string(result.exit_code);
        case Termination::Signaled:
            return "was killed by signal " + std::to_string(result.signal);
        case Termination::StoppedByProcwatch:
            return "was stopped by procwatch";
    }

    throw std::invalid_argument("Unknown process termination kind");
}

// Creates a new, uniquely named file next to the report and returns its
// descriptor. The name combines the process ID with a counter, and
// O_EXCL guarantees that an existing file is never reused
int create_temp_file(
    const std::filesystem::path& path,
    std::filesystem::path& temp_path
)
{
    static std::atomic<unsigned long> counter{0};

    for (int attempt = 0; attempt < max_temp_file_attempts; ++attempt)
    {
        temp_path = path;
        temp_path += "." + std::to_string(::getpid()) + "." +
                     std::to_string(counter++) + ".tmp";

        // 0666 lets the umask decide the final permissions, as for a
        // normally created file
        const int fd = ::open(
            temp_path.c_str(),
            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
            0666
        );

        if (fd >= 0)
        {
            return fd;
        }

        if (errno != EEXIST)
        {
            throw std::runtime_error(
                "Cannot create a temporary file for " + path.string() +
                ": " + error_message(errno)
            );
        }
    }

    throw std::runtime_error(
        "Cannot find an unused temporary file name for " + path.string()
    );
}

// Writes the whole buffer and returns 0 or the errno of the failure
int write_all(int fd, const std::string& content)
{
    std::size_t written = 0;

    while (written < content.size())
    {
        const ssize_t count = ::write(
            fd, content.data() + written, content.size() - written);

        if (count < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return errno;
        }

        // No progress and no error would otherwise loop forever
        if (count == 0)
        {
            return EIO;
        }

        written += static_cast<std::size_t>(count);
    }

    return 0;
}

} // namespace

// Builds the complete JSON report from validated report data
nlohmann::json build_report(const ReportData& data, const Config& config)
{
    validate_report_data(data);

    return {
        {"report_version", report_version},
        {"started_at", format_utc(data.started_at)},
        {"duration_seconds", offset_seconds(data.end_time, data.start_time)},
        {"config", config_to_json(config)},
        {"process", process_to_json(data.process_result)},
        {"samples", samples_to_json(data)},
        {"findings", findings_to_json(data)},
        {"stable", data.stable}
    };
}

// Writes the report through a temporary file in the same directory
void write_report(
    const nlohmann::json& report,
    const std::filesystem::path& path
)
{
    // Invalid UTF-8, in command arguments, is replaced instead
    // of making the whole report fail
    const std::string content =
        report.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) +
        "\n";

    std::filesystem::path temp_path;
    int fd = create_temp_file(path, temp_path);

    // Closes and removes the temporary file, then reports the failure
    const auto fail = [&fd, &temp_path](const std::string& message)
    {
        if (fd >= 0)
        {
            ::close(fd);
        }

        std::error_code ignored;
        std::filesystem::remove(temp_path, ignored);
        throw std::runtime_error(message);
    };

    if (const int error = write_all(fd, content); error != 0)
    {
        fail("Failed to write " + temp_path.string() + ": " +
             error_message(error));
    }

    // Make sure the content is on disk before the rename makes it visible
    if (::fsync(fd) != 0)
    {
        fail("Failed to write " + temp_path.string() + ": " +
             error_message(errno));
    }

    const int close_result = ::close(fd);
    fd = -1;

    if (close_result != 0)
    {
        fail("Failed to write " + temp_path.string() + ": " +
             error_message(errno));
    }

    // Renaming within one directory replaces the report in a single step
    std::error_code error;
    std::filesystem::rename(temp_path, path, error);

    if (error)
    {
        fail("Cannot write report to " + path.string() + ": " +
             error.message());
    }
}

// Formats the verdict, process end and findings for the console
std::string format_summary(const ReportData& data)
{
    validate_report_data(data);

    std::ostringstream summary;

    summary << "Process " << describe_termination(data.process_result)
            << " after "
            << format_number(offset_seconds(data.end_time, data.start_time))
            << " s\n";
    summary << "Samples: " << data.samples.size() << '\n';
    summary << "Findings: " << data.findings.size() << '\n';

    for (const Finding& finding : data.findings)
    {
        summary << "  ["
                << format_number(
                       offset_seconds(finding.timestamp, data.start_time))
                << " s] " << finding.type << ": " << finding.description
                << '\n';
    }

    summary << "Verdict: " << (data.stable ? "stable" : "unstable") << '\n';

    return summary.str();
}

}