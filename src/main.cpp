#include "procwatch/config.hpp"
#include "procwatch/report.hpp"
#include "procwatch/supervisor.hpp"
#include "procwatch/types.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include <unistd.h>

namespace
{

using procwatch::Config;
using procwatch::ExitCode;

// Shorter sampling intervals are too coarse for the 10 ms CPU clock tick
constexpr std::int64_t min_sampling_interval_ms = 100;

// 30 days keeps all time calculations far from overflowing
constexpr std::int64_t max_duration_value_ms = 2'592'000'000;

constexpr std::string_view report_path_option = "--report-path";

// A command-line option holding a duration in milliseconds
struct DurationOption
{
    std::string_view name;
    std::chrono::milliseconds Config::*field;
    std::int64_t minimum_ms;
};

// A command-line option holding a percentage
struct PercentOption
{
    std::string_view name;
    double Config::*field;
};

constexpr DurationOption duration_options[] = {
    {"--sampling-interval-ms", &Config::sampling_interval, min_sampling_interval_ms},
    {"--max-duration-ms", &Config::max_duration, min_sampling_interval_ms},
    {"--warmup-duration-ms", &Config::warmup_duration, 0},
    {"--cpu-sustained-duration-ms", &Config::cpu_sustained_duration, 0}
};

constexpr PercentOption percent_options[] = {
    {"--memory-growth-threshold-percent", &Config::memory_growth_threshold_percent},
    {"--cpu-usage-threshold-percent", &Config::cpu_usage_threshold_percent}
};

constexpr std::string_view usage_text =
R"(Usage: procwatch [options] -- <command> [args...]

Launches <command> as a child process, monitors its memory and CPU usage,
and writes a JSON report.

Options:
  --sampling-interval-ms <n>
      Time between samples in milliseconds, at least 100 (default: 1000)
  --max-duration-ms <n>
      Maximum monitoring time in milliseconds, at least 100 (default: 60000)
  --warmup-duration-ms <n>
      Start-up time ignored by the analysis in milliseconds (default: 5000)
  --cpu-sustained-duration-ms <n>
      How long CPU usage must stay above the threshold in milliseconds
      (default: 10000)
  --memory-growth-threshold-percent <x>
      Allowed RSS growth above the baseline in percent (default: 20)
  --cpu-usage-threshold-percent <x>
      CPU usage threshold in percent of one logical CPU; values above 100
      are valid for multi-threaded programs (default: 90)
  --report-path <path>
      Where to write the JSON report (default: procwatch-report.json)
  -h, --help
      Show this help and exit

Durations can be at most 2592000000 ms (30 days). The warm-up, one sampling
interval and the longer of the sampling interval and the CPU sustained duration
must together fit into the maximum duration.

Stopping:
  Monitoring ends when the command exits, when the maximum duration is reached,
  or when procwatch receives SIGINT or SIGTERM. A command that ends after
  procwatch has observed a stop request counts as stopped by procwatch.

  Ctrl+C reaches procwatch and the command at the same time. If the command
  ends from that signal before procwatch has observed the stop request, it is
  reported as an abnormal termination and procwatch exits with code 1.

Exit codes:
  0  No stability issue detected
  1  One or more stability issues detected
  2  Invalid usage or configuration, or the command could not be executed
  3  Internal error
)";

// An invalid command line or configuration
class UsageError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

int to_int(ExitCode code)
{
    return static_cast<int>(code);
}

// Returns the option with the given name, or nullptr if there is none
template <typename Option, std::size_t Size>
const Option* find_option(const Option (&options)[Size], std::string_view name)
{
    const auto* const end = std::end(options);
    const auto* const option = std::find_if(
        std::begin(options),
        end,
        [name](const Option& candidate)
        {
            return candidate.name == name;
        }
    );

    return option == end ? nullptr : option;
}

UsageError invalid_value(
    std::string_view name,
    std::string_view value,
    const std::string& expected
)
{
    return UsageError(
        "invalid value for " + std::string(name) + ": '" +
        std::string(value) + "' (expected " + expected + ")"
    );
}

// Parses a whole number of milliseconds within the option's range
std::chrono::milliseconds parse_duration(
    const DurationOption& option,
    std::string_view value
)
{
    std::int64_t milliseconds = 0;
    const char* const end = value.data() + value.size();
    const auto result = std::from_chars(value.data(), end, milliseconds);

    if (result.ec != std::errc{} || result.ptr != end ||
        milliseconds < option.minimum_ms ||
        milliseconds > max_duration_value_ms)
    {
        throw invalid_value(
            option.name,
            value,
            "whole milliseconds from " + std::to_string(option.minimum_ms) +
                " to " + std::to_string(max_duration_value_ms)
        );
    }

    return std::chrono::milliseconds(milliseconds);
}

// Parses a finite, non-negative decimal number
double parse_percent(const PercentOption& option, std::string_view value)
{
    double percent = 0.0;
    const char* const end = value.data() + value.size();
    const auto result = std::from_chars(
        value.data(), end, percent, std::chars_format::fixed);

    if (result.ec != std::errc{} || result.ptr != end ||
        !std::isfinite(percent) || std::signbit(percent))
    {
        throw invalid_value(
            option.name, value, "a non-negative decimal number");
    }

    return percent;
}

// Rejects settings that would leave a check unable to run, because such a
// run could wrongly look stable
void validate_monitoring_window(const Config& config)
{
    // The first sample after the warm-up can come up to one interval late.
    // Memory then needs one more sample, and CPU the sustained duration
    const auto required =
        config.warmup_duration + config.sampling_interval +
        std::max(config.sampling_interval, config.cpu_sustained_duration);

    if (required > config.max_duration)
    {
        throw UsageError(
            "--max-duration-ms is too short: the warm-up and analysis need " +
            std::to_string(required.count()) + " ms but only " +
            std::to_string(config.max_duration.count()) + " ms are available"
        );
    }
}

// Catches an unusable report path before the target is started
void validate_report_path(const std::filesystem::path& path)
{
    const std::string option(report_path_option);

    if (!path.has_filename())
    {
        throw UsageError(option + " must name a file: '" + path.string() + "'");
    }

    std::error_code error;

    if (std::filesystem::is_directory(path, error))
    {
        throw UsageError(option + " is a directory: '" + path.string() + "'");
    }

    const std::filesystem::path directory =
        path.has_parent_path() ? path.parent_path() : ".";

    if (!std::filesystem::is_directory(directory, error))
    {
        throw UsageError(
            option + " directory does not exist: '" + directory.string() + "'"
        );
    }

    if (::access(directory.c_str(), W_OK | X_OK) != 0)
    {
        throw UsageError(
            option + " directory is not writable: '" + directory.string() + "'"
        );
    }
}

// Builds the configuration from the command line.
// Returns std::nullopt if the help text was requested
std::optional<Config> parse_arguments(int argc, char* argv[])
{
    Config config;
    std::set<std::string_view> seen_options;
    bool command_found = false;

    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument = argv[index];

        // Everything after the separator belongs to the target
        if (argument == "--")
        {
            config.command.assign(argv + index + 1, argv + argc);
            command_found = true;
            break;
        }

        if (argument == "-h" || argument == "--help")
        {
            return std::nullopt;
        }

        if (argument.empty() || argument.front() != '-')
        {
            throw UsageError(
                "unexpected argument '" + std::string(argument) +
                "' (use '--' before the command)"
            );
        }

        // Options accept both --name=value and --name value
        const std::size_t equals = argument.find('=');
        const std::string_view name = argument.substr(0, equals);

        const auto* const duration = find_option(duration_options, name);
        const auto* const percent = find_option(percent_options, name);

        if (duration == nullptr && percent == nullptr &&
            name != report_path_option)
        {
            throw UsageError("unknown option '" + std::string(name) + "'");
        }

        if (!seen_options.insert(name).second)
        {
            throw UsageError("repeated option '" + std::string(name) + "'");
        }

        std::string_view value;

        if (equals != std::string_view::npos)
        {
            value = argument.substr(equals + 1);
        }
        else if (index + 1 < argc && std::string_view(argv[index + 1]) != "--")
        {
            value = argv[++index];
        }
        else
        {
            throw UsageError("missing value for " + std::string(name));
        }

        if (duration != nullptr)
        {
            config.*(duration->field) = parse_duration(*duration, value);
        }
        else if (percent != nullptr)
        {
            config.*(percent->field) = parse_percent(*percent, value);
        }
        else
        {
            config.report_path = std::filesystem::path(value);
        }
    }

    if (!command_found)
    {
        throw UsageError("missing '--' and the command to monitor");
    }

    if (config.command.empty() || config.command.front().empty())
    {
        throw UsageError("missing command after '--'");
    }

    validate_monitoring_window(config);
    validate_report_path(config.report_path);

    return config;
}

// Runs one monitoring session and returns the exit code for its result
int monitor(const Config& config)
{
    procwatch::Supervisor supervisor(config);
    const procwatch::ReportData data = supervisor.run();

    // The target has ended, so ignoring SIGPIPE cannot affect it. A closed
    // stdout must not prevent the report from being written
    std::signal(SIGPIPE, SIG_IGN);

    const nlohmann::json report = procwatch::build_report(data, config);

    std::cout << procwatch::format_summary(data) << std::flush;

    procwatch::write_report(report, config.report_path);

    std::cout << "Report: " << config.report_path.string() << std::endl;

    return to_int(data.stable ? ExitCode::Success : ExitCode::StabilityIssue);
}

void print_error(const std::string& message)
{
    std::cerr << "procwatch: error: " << message << std::endl;
}

} // namespace

int main(int argc, char* argv[])
{
    try
    {
        const std::optional<Config> config = parse_arguments(argc, argv);

        if (!config)
        {
            std::cout << usage_text;
            return to_int(ExitCode::Success);
        }

        return monitor(*config);
    }
    catch (const UsageError& error)
    {
        print_error(error.what());
        std::cerr << "Try 'procwatch --help' for more information." << std::endl;
        return to_int(ExitCode::UsageError);
    }
    catch (const procwatch::LaunchError& error)
    {
        print_error(error.what());
        return to_int(ExitCode::UsageError);
    }
    catch (const std::exception& error)
    {
        print_error(error.what());
        return to_int(ExitCode::InternalError);
    }
    catch (...)
    {
        print_error("unknown internal error");
        return to_int(ExitCode::InternalError);
    }
}