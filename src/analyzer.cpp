#include "procwatch/analyzer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace procwatch
{

// Internal helpers used only by this source file
namespace
{

using Clock = std::chrono::steady_clock;
using Seconds = std::chrono::duration<double>;

constexpr double bytes_per_mib = 1024.0 * 1024.0;

// Allowed relative difference between the tick rates implied by two samples.
// ProcReader computes seconds by dividing ticks by a fixed rate, so real
// samples differ only by floating-point rounding
constexpr double tick_rate_tolerance = 1e-9;

// A run of consecutive counted intervals with CPU usage above the threshold
struct CpuEpisode
{
    Clock::time_point start;
    Clock::time_point end;
    double cpu_seconds{0.0};
    std::optional<Clock::time_point> confirmed_at;
};

// Formats a value with one decimal place
std::string format_number(double value)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(1) << value;
    return stream.str();
}

// Formats a byte count in MiB with one decimal place
std::string format_mib(std::uint64_t bytes)
{
    return format_number(static_cast<double>(bytes) / bytes_per_mib) + " MiB";
}

// Returns true for a finite, non-negative threshold
bool is_valid_threshold(double value)
{
    return std::isfinite(value) && value >= 0.0;
}

// Checks the CPU values of a single sample.
// Ticks and seconds describe the same CPU time, so they must be zero
// together and imply the same tick rate in every sample
void validate_cpu_metrics(
    const ProcessMetrics& metrics,
    std::optional<double>& ticks_per_second
)
{
    if (!std::isfinite(metrics.cpu_time_seconds) ||
        metrics.cpu_time_seconds < 0.0)
    {
        throw std::invalid_argument(
            "Sample CPU time must be finite and non-negative"
        );
    }

    if ((metrics.cpu_time_ticks == 0) != (metrics.cpu_time_seconds == 0.0))
    {
        throw std::invalid_argument(
            "Sample CPU ticks and seconds are inconsistent"
        );
    }

    if (metrics.cpu_time_ticks == 0)
    {
        return;
    }

    const double rate =
        static_cast<double>(metrics.cpu_time_ticks) / metrics.cpu_time_seconds;

    if (!ticks_per_second)
    {
        ticks_per_second = rate;
    }
    else if (std::abs(rate - *ticks_per_second) >
             *ticks_per_second * tick_rate_tolerance)
    {
        throw std::invalid_argument(
            "Sample CPU ticks and seconds are inconsistent"
        );
    }
}

// Checks the complete sample sequence before any analysis is done
void validate_samples(const std::vector<Sample>& samples)
{
    std::optional<double> ticks_per_second;

    for (std::size_t i = 0; i < samples.size(); ++i)
    {
        const Sample& current = samples[i];
        validate_cpu_metrics(current.metrics, ticks_per_second);

        if (i == 0)
        {
            continue;
        }

        const Sample& previous = samples[i - 1];

        if (current.timestamp <= previous.timestamp)
        {
            throw std::invalid_argument(
                "Sample timestamps must be strictly increasing"
            );
        }

        if (current.metrics.cpu_time_ticks < previous.metrics.cpu_time_ticks ||
            current.metrics.cpu_time_seconds < previous.metrics.cpu_time_seconds)
        {
            throw std::invalid_argument("Sample CPU time must not decrease");
        }
    }
}

} // namespace

// Copies and validates the thresholds used for the analysis
Analyzer::Analyzer(const Config& config)
    : warmup_duration_(config.warmup_duration),
      cpu_sustained_duration_(config.cpu_sustained_duration),
      memory_growth_threshold_percent_(config.memory_growth_threshold_percent),
      cpu_usage_threshold_percent_(config.cpu_usage_threshold_percent)
{
    if (!is_valid_threshold(memory_growth_threshold_percent_))
    {
        throw std::invalid_argument(
            "Memory growth threshold must be a finite, non-negative number"
        );
    }

    if (!is_valid_threshold(cpu_usage_threshold_percent_))
    {
        throw std::invalid_argument(
            "CPU usage threshold must be a finite, non-negative number"
        );
    }

    if (warmup_duration_ < std::chrono::milliseconds::zero())
    {
        throw std::invalid_argument("Warm-up duration must not be negative");
    }

    if (cpu_sustained_duration_ < std::chrono::milliseconds::zero())
    {
        throw std::invalid_argument(
            "CPU sustained duration must not be negative"
        );
    }
}

// Validates the samples, runs all checks and returns the findings in time order
std::vector<Finding> Analyzer::analyze(const std::vector<Sample>& samples) const
{
    validate_samples(samples);

    std::vector<Finding> findings = find_sustained_cpu(samples);

    if (auto memory_finding = find_memory_growth(samples))
    {
        findings.push_back(std::move(*memory_finding));
    }

    std::stable_sort(
        findings.begin(),
        findings.end(),
        [](const Finding& left, const Finding& right)
        {
            return left.timestamp < right.timestamp;
        }
    );

    return findings;
}

// Reports RSS growth above the threshold relative to a fixed baseline.
// Returns at most one finding, because repeated crossings of the same
// baseline describe the same condition
std::optional<Finding> Analyzer::find_memory_growth(
    const std::vector<Sample>& samples
) const
{
    if (samples.size() < 2)
    {
        return std::nullopt;
    }

    // The warm-up starts at the first sample, which is taken right after
    // the process starts
    const Clock::time_point warmup_end =
        samples.front().timestamp + warmup_duration_;

    // The baseline is the first sample at or after the warm-up end with a
    // non-zero RSS. It is chosen once and never recalculated
    const auto baseline = std::find_if(
        samples.begin(),
        samples.end(),
        [warmup_end](const Sample& sample)
        {
            return sample.timestamp >= warmup_end &&
                   sample.metrics.rss_bytes > 0;
        }
    );

    if (baseline == samples.end())
    {
        return std::nullopt;
    }

    const double baseline_rss =
        static_cast<double>(baseline->metrics.rss_bytes);

    const Sample* first_crossing = nullptr;
    double first_growth_percent = 0.0;
    std::uint64_t peak_rss = baseline->metrics.rss_bytes;

    for (auto it = std::next(baseline); it != samples.end(); ++it)
    {
        const double rss = static_cast<double>(it->metrics.rss_bytes);

        // Multiplying before dividing keeps whole-percent values exact
        const double growth_percent =
            (rss - baseline_rss) * 100.0 / baseline_rss;

        if (first_crossing == nullptr &&
            growth_percent > memory_growth_threshold_percent_)
        {
            first_crossing = &*it;
            first_growth_percent = growth_percent;
        }

        peak_rss = std::max(peak_rss, it->metrics.rss_bytes);
    }

    if (first_crossing == nullptr)
    {
        return std::nullopt;
    }

    Finding finding;
    finding.type = memory_growth_finding;
    finding.timestamp = first_crossing->timestamp;
    finding.description =
        "RSS grew " + format_number(first_growth_percent) +
        "% above the baseline (" +
        format_mib(baseline->metrics.rss_bytes) + " -> " +
        format_mib(first_crossing->metrics.rss_bytes) + ", peak " +
        format_mib(peak_rss) + "); threshold " +
        format_number(memory_growth_threshold_percent_) + "%";

    return finding;
}

// Reports each continuous period after the warm-up in which CPU usage
// stayed above the threshold for at least the sustained duration
std::vector<Finding> Analyzer::find_sustained_cpu(
    const std::vector<Sample>& samples
) const
{
    std::vector<Finding> findings;

    if (samples.size() < 2)
    {
        return findings;
    }

    const Clock::time_point warmup_end =
        samples.front().timestamp + warmup_duration_;

    std::optional<CpuEpisode> episode;

    // Ends the current episode and reports it if it lasted long enough
    const auto close_episode = [&]()
    {
        if (episode && episode->confirmed_at)
        {
            const double duration =
                Seconds(episode->end - episode->start).count();

            Finding finding;
            finding.type = sustained_cpu_finding;
            finding.timestamp = *episode->confirmed_at;
            finding.description =
                "CPU usage stayed above " +
                format_number(cpu_usage_threshold_percent_) + "% for " +
                format_number(duration) + " s (average " +
                format_number(episode->cpu_seconds / duration * 100.0) +
                "%); required " +
                format_number(Seconds(cpu_sustained_duration_).count()) +
                " s";

            findings.push_back(std::move(finding));
        }

        episode.reset();
    };

    for (std::size_t i = 1; i < samples.size(); ++i)
    {
        const Sample& previous = samples[i - 1];
        const Sample& current = samples[i];

        // Only the part of the interval after the warm-up is counted.
        // Intervals that end at or before the warm-up end are skipped
        const Clock::time_point counted_start =
            std::max(previous.timestamp, warmup_end);

        if (counted_start >= current.timestamp)
        {
            continue;
        }

        // validate_samples() guarantees a positive interval and a
        // non-decreasing CPU time
        const double interval_seconds =
            Seconds(current.timestamp - previous.timestamp).count();
        const double interval_cpu_seconds =
            current.metrics.cpu_time_seconds - previous.metrics.cpu_time_seconds;

        // The samples cannot show how CPU time was spread within the
        // interval, so it is assumed to be even. The counted part then has
        // the same usage as the whole interval. Usage is relative to one
        // CPU, so multi-threaded processes can exceed 100%
        const double usage_percent =
            interval_cpu_seconds / interval_seconds * 100.0;

        if (usage_percent <= cpu_usage_threshold_percent_)
        {
            close_episode();
            continue;
        }

        // CPU time that falls into the counted part of the interval
        const double counted_fraction =
            Seconds(current.timestamp - counted_start).count() /
            interval_seconds;

        if (!episode)
        {
            episode = CpuEpisode{counted_start, counted_start, 0.0, {}};
        }

        episode->end = current.timestamp;
        episode->cpu_seconds += interval_cpu_seconds * counted_fraction;

        // Record the first sample at which the episode reached the
        // sustained duration
        if (!episode->confirmed_at &&
            episode->end - episode->start >= cpu_sustained_duration_)
        {
            episode->confirmed_at = current.timestamp;
        }
    }

    // An episode that is still running at the last sample is reported too
    close_episode();

    return findings;
}

}