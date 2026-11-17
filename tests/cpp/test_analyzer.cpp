#include "procwatch/analyzer.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{

using namespace std::chrono_literals;

using procwatch::Analyzer;
using procwatch::Config;
using procwatch::Finding;
using procwatch::Sample;

using Clock = std::chrono::steady_clock;

constexpr std::uint64_t mib = 1024 * 1024;

// Tick rate used to keep cpu_time_ticks consistent with cpu_time_seconds
constexpr double ticks_per_second = 100.0;

constexpr double nan = std::numeric_limits<double>::quiet_NaN();
constexpr double infinity = std::numeric_limits<double>::infinity();

// A fixed, arbitrary start time that keeps the tests independent of the clock
const Clock::time_point t0 = Clock::time_point{} + 1h;

// Builds one sample at t0 + offset with consistent CPU ticks and seconds
Sample make_sample(
    std::chrono::milliseconds offset,
    std::uint64_t rss_mib,
    double cpu_seconds)
{
    Sample sample;
    sample.timestamp = t0 + offset;
    sample.metrics.rss_bytes = rss_mib * mib;
    sample.metrics.cpu_time_seconds = cpu_seconds;
    sample.metrics.cpu_time_ticks = static_cast<std::uint64_t>(
        std::llround(cpu_seconds * ticks_per_second));
    return sample;
}

// Builds one sample per second with the given RSS values and no CPU time
std::vector<Sample> rss_series(const std::vector<std::uint64_t>& rss_mib)
{
    std::vector<Sample> samples;

    for (std::size_t i = 0; i < rss_mib.size(); ++i)
    {
        samples.push_back(make_sample(
            std::chrono::seconds(static_cast<long>(i)), rss_mib[i], 0.0));
    }

    return samples;
}

// Builds one sample per second whose consecutive intervals have the given
// CPU usage in percent. The tests use values that are exact in binary,
// so the boundary checks are not affected by rounding
std::vector<Sample> cpu_series(const std::vector<double>& usage_percent)
{
    std::vector<Sample> samples;
    double cpu_seconds = 0.0;

    samples.push_back(make_sample(0s, 100, cpu_seconds));

    for (std::size_t i = 0; i < usage_percent.size(); ++i)
    {
        cpu_seconds += usage_percent[i] / 100.0;
        samples.push_back(make_sample(
            std::chrono::seconds(static_cast<long>(i + 1)), 100, cpu_seconds));
    }

    return samples;
}

// Memory checks: no warm-up and a 20% threshold unless a test changes them
Config memory_config()
{
    Config config;
    config.warmup_duration = 0ms;
    config.memory_growth_threshold_percent = 20.0;
    return config;
}

// CPU checks: no warm-up and a 50% threshold over 3 seconds unless a test
// changes them
Config cpu_config()
{
    Config config;
    config.warmup_duration = 0ms;
    config.cpu_usage_threshold_percent = 50.0;
    config.cpu_sustained_duration = 3s;
    return config;
}

bool contains(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

// Construction

// Checks that negative, NaN and infinite memory thresholds are rejected
TEST(AnalyzerConstruction, RejectsInvalidMemoryThreshold)
{
    for (const double value : {-1.0, nan, infinity, -infinity})
    {
        Config config;
        config.memory_growth_threshold_percent = value;

        EXPECT_THROW(Analyzer{config}, std::invalid_argument) << value;
    }
}

// Checks that negative, NaN and infinite CPU thresholds are rejected
TEST(AnalyzerConstruction, RejectsInvalidCpuThreshold)
{
    for (const double value : {-1.0, nan, infinity, -infinity})
    {
        Config config;
        config.cpu_usage_threshold_percent = value;

        EXPECT_THROW(Analyzer{config}, std::invalid_argument) << value;
    }
}

// Checks that negative durations are rejected
TEST(AnalyzerConstruction, RejectsNegativeDurations)
{
    Config config;
    config.warmup_duration = -1ms;
    EXPECT_THROW(Analyzer{config}, std::invalid_argument);

    config = Config{};
    config.cpu_sustained_duration = -1ms;
    EXPECT_THROW(Analyzer{config}, std::invalid_argument);
}

// Checks that zero thresholds and durations are accepted
TEST(AnalyzerConstruction, AcceptsZeroValues)
{
    Config config;
    config.memory_growth_threshold_percent = 0.0;
    config.cpu_usage_threshold_percent = 0.0;
    config.warmup_duration = 0ms;
    config.cpu_sustained_duration = 0ms;

    EXPECT_NO_THROW(Analyzer{config});
}

// Sample validation

// Checks that empty input and a single sample produce no findings
TEST(AnalyzerValidation, AcceptsFewerThanTwoSamples)
{
    const Analyzer analyzer(cpu_config());

    EXPECT_TRUE(analyzer.analyze({}).empty());
    EXPECT_TRUE(analyzer.analyze({make_sample(0s, 100, 50.0)}).empty());
}

// Checks that duplicate timestamps are rejected
TEST(AnalyzerValidation, RejectsDuplicateTimestamps)
{
    const std::vector<Sample> samples{
        make_sample(0s, 100, 0.0),
        make_sample(1s, 100, 0.5),
        make_sample(1s, 100, 0.5)};

    EXPECT_THROW(Analyzer(cpu_config()).analyze(samples), std::invalid_argument);
}

// Checks that out-of-order timestamps are rejected
TEST(AnalyzerValidation, RejectsOutOfOrderTimestamps)
{
    const std::vector<Sample> samples{
        make_sample(0s, 100, 0.0),
        make_sample(2s, 100, 0.5),
        make_sample(1s, 100, 0.75)};

    EXPECT_THROW(Analyzer(cpu_config()).analyze(samples), std::invalid_argument);
}

// Checks that NaN, infinite and negative CPU seconds are rejected,
// also when there is only one sample
TEST(AnalyzerValidation, RejectsInvalidCpuSeconds)
{
    for (const double value : {nan, infinity, -0.25})
    {
        std::vector<Sample> samples = cpu_series({25});
        samples[1].metrics.cpu_time_seconds = value;
        EXPECT_THROW(
            Analyzer(cpu_config()).analyze(samples), std::invalid_argument)
            << value;

        std::vector<Sample> single{make_sample(0s, 100, 0.25)};
        single[0].metrics.cpu_time_seconds = value;
        EXPECT_THROW(
            Analyzer(cpu_config()).analyze(single), std::invalid_argument)
            << value;
    }
}

// Checks that CPU time moving backwards is rejected
TEST(AnalyzerValidation, RejectsDecreasingCpuTime)
{
    const std::vector<Sample> samples{
        make_sample(0s, 100, 0.0),
        make_sample(1s, 100, 0.75),
        make_sample(2s, 100, 0.5)};

    EXPECT_THROW(Analyzer(cpu_config()).analyze(samples), std::invalid_argument);
}

// Checks that ticks and seconds must be zero together
TEST(AnalyzerValidation, RejectsTicksAndSecondsThatDisagreeOnZero)
{
    std::vector<Sample> samples = cpu_series({25});
    samples[0].metrics.cpu_time_seconds = 0.1;
    EXPECT_THROW(Analyzer(cpu_config()).analyze(samples), std::invalid_argument);

    samples = cpu_series({25, 25});
    samples[1].metrics.cpu_time_seconds = 0.0;
    EXPECT_THROW(Analyzer(cpu_config()).analyze(samples), std::invalid_argument);
}

// Checks that samples implying different tick rates are rejected
TEST(AnalyzerValidation, RejectsInconsistentTickRate)
{
    // 0.5 s with 51 ticks does not match the 100 ticks per second of the
    // other samples
    std::vector<Sample> samples = cpu_series({25, 25});
    samples[2].metrics.cpu_time_ticks = 51;

    EXPECT_THROW(Analyzer(cpu_config()).analyze(samples), std::invalid_argument);
}

// Checks that seconds computed from ticks, as ProcReader does, are always
// accepted despite floating-point rounding
TEST(AnalyzerValidation, AcceptsSecondsComputedFromTicks)
{
    for (const long rate : {100L, 250L, 1000L})
    {
        std::vector<Sample> samples;

        for (std::uint64_t ticks = 0; ticks < 20000; ticks += 7)
        {
            Sample sample;
            sample.timestamp =
                t0 + std::chrono::milliseconds(static_cast<long>(ticks));
            sample.metrics.rss_bytes = 100 * mib;
            sample.metrics.cpu_time_ticks = ticks;
            sample.metrics.cpu_time_seconds =
                static_cast<double>(ticks) / static_cast<double>(rate);
            samples.push_back(sample);
        }

        EXPECT_NO_THROW(Analyzer(cpu_config()).analyze(samples)) << rate;
    }
}

// Memory growth

// Checks that growth equal to the threshold is not reported
TEST(AnalyzerMemory, ReportsNothingForGrowthEqualToThreshold)
{
    EXPECT_TRUE(
        Analyzer(memory_config()).analyze(rss_series({100, 120})).empty());
}

// Checks that growth above the threshold is reported at the first crossing
TEST(AnalyzerMemory, ReportsGrowthAboveThreshold)
{
    const auto findings =
        Analyzer(memory_config()).analyze(rss_series({100, 110, 121, 125}));

    ASSERT_EQ(findings.size(), 1u);
    EXPECT_EQ(findings[0].type, procwatch::memory_growth_finding);
    EXPECT_EQ(findings[0].timestamp, t0 + 2s);
    EXPECT_TRUE(contains(findings[0].description, "21.0%"))
        << findings[0].description;
    EXPECT_TRUE(contains(findings[0].description, "peak 125.0 MiB"))
        << findings[0].description;
}

// Checks that decreasing memory is not reported
TEST(AnalyzerMemory, ReportsNothingForDecreasingMemory)
{
    EXPECT_TRUE(
        Analyzer(memory_config()).analyze(rss_series({100, 50})).empty());
}

// Checks that the baseline stays at the first value and is not lowered
// when RSS drops later
TEST(AnalyzerMemory, KeepsFixedBaseline)
{
    // 110 is 22% above 90 but only 10% above the original baseline of 100
    EXPECT_TRUE(
        Analyzer(memory_config()).analyze(rss_series({100, 90, 110})).empty());
}

// Checks that repeated crossings produce only one finding with the peak
TEST(AnalyzerMemory, ReportsRepeatedCrossingsOnce)
{
    const auto findings =
        Analyzer(memory_config()).analyze(rss_series({100, 130, 100, 140}));

    ASSERT_EQ(findings.size(), 1u);
    EXPECT_EQ(findings[0].timestamp, t0 + 1s);
    EXPECT_TRUE(contains(findings[0].description, "peak 140.0 MiB"))
        << findings[0].description;
}

// Checks that a zero RSS is skipped as a baseline candidate
TEST(AnalyzerMemory, SkipsZeroRssAsBaseline)
{
    const auto findings =
        Analyzer(memory_config()).analyze(rss_series({0, 100, 125}));

    ASSERT_EQ(findings.size(), 1u);
    EXPECT_EQ(findings[0].timestamp, t0 + 2s);
    EXPECT_TRUE(contains(findings[0].description, "100.0 MiB -> 125.0 MiB"))
        << findings[0].description;
}

// Memory warm-up

// Checks that nothing is reported when all samples are inside the warm-up
TEST(AnalyzerMemoryWarmup, ReportsNothingWhenAllSamplesAreInsideWarmup)
{
    Config config = memory_config();
    config.warmup_duration = 10s;

    EXPECT_TRUE(
        Analyzer(config).analyze(rss_series({100, 200, 300})).empty());
}

// Checks that a baseline without any later sample is not enough
TEST(AnalyzerMemoryWarmup, ReportsNothingWhenOnlyBaselineFollowsWarmup)
{
    Config config = memory_config();
    config.warmup_duration = 2s;

    EXPECT_TRUE(
        Analyzer(config).analyze(rss_series({100, 100, 500})).empty());
}

// Checks that the sample exactly at the end of the warm-up is the baseline
TEST(AnalyzerMemoryWarmup, UsesSampleAtWarmupEndAsBaseline)
{
    Config config = memory_config();
    config.warmup_duration = 2s;

    const auto findings =
        Analyzer(config).analyze(rss_series({50, 50, 100, 125}));

    ASSERT_EQ(findings.size(), 1u);
    EXPECT_EQ(findings[0].timestamp, t0 + 3s);
    EXPECT_TRUE(contains(findings[0].description, "100.0 MiB -> 125.0 MiB"))
        << findings[0].description;
}

// Checks that growth during the warm-up does not count
TEST(AnalyzerMemoryWarmup, IgnoresGrowthDuringWarmup)
{
    Config config = memory_config();
    config.warmup_duration = 2s;

    EXPECT_TRUE(
        Analyzer(config).analyze(rss_series({10, 50, 50, 55})).empty());
}

// CPU utilization

// Checks that usage below or equal to the threshold is not reported
TEST(AnalyzerCpu, ReportsNothingAtOrBelowThreshold)
{
    const Analyzer analyzer(cpu_config());

    EXPECT_TRUE(analyzer.analyze(cpu_series({25, 25, 25, 25, 25})).empty());
    EXPECT_TRUE(analyzer.analyze(cpu_series({50, 50, 50, 50, 50})).empty());
}

// Checks that usage above 100% from multi-threaded processes is supported
TEST(AnalyzerCpu, SupportsUsageAboveOneHundredPercent)
{
    Config config = cpu_config();
    config.cpu_usage_threshold_percent = 150.0;

    const auto findings =
        Analyzer(config).analyze(cpu_series({250, 250, 250, 250}));

    ASSERT_EQ(findings.size(), 1u);
    EXPECT_EQ(findings[0].type, procwatch::sustained_cpu_finding);
    EXPECT_TRUE(contains(findings[0].description, "average 250.0%"))
        << findings[0].description;
}

// Checks that usage is calculated from the real elapsed time when samples
// are not evenly spaced
TEST(AnalyzerCpu, UsesElapsedTimeBetweenSamples)
{
    // 2 s of CPU time over the first 2 s gap is 100%
    const std::vector<Sample> samples{
        make_sample(0s, 100, 0.0),
        make_sample(2s, 100, 2.0),
        make_sample(3s, 100, 3.0),
        make_sample(4s, 100, 4.0)};

    const auto findings = Analyzer(cpu_config()).analyze(samples);

    ASSERT_EQ(findings.size(), 1u);
    EXPECT_EQ(findings[0].timestamp, t0 + 3s);
    EXPECT_TRUE(contains(findings[0].description, "average 100.0%"))
        << findings[0].description;
}

// Sustained CPU duration

// Checks that high usage shorter than the sustained duration is ignored
TEST(AnalyzerCpuSustained, ReportsNothingForShortHighUsage)
{
    EXPECT_TRUE(Analyzer(cpu_config())
                    .analyze(cpu_series({25, 75, 75, 25, 25}))
                    .empty());
}

// Checks that high usage lasting exactly the sustained duration is reported
// when the duration is first reached
TEST(AnalyzerCpuSustained, ReportsHighUsageLastingExactlySustainedDuration)
{
    const auto findings =
        Analyzer(cpu_config()).analyze(cpu_series({25, 75, 75, 75, 25}));

    ASSERT_EQ(findings.size(), 1u);
    EXPECT_EQ(findings[0].timestamp, t0 + 4s);
}

// Checks that a long episode produces one finding with its full duration
TEST(AnalyzerCpuSustained, ReportsLongEpisodeOnce)
{
    const auto findings = Analyzer(cpu_config()).analyze(
        cpu_series({75, 75, 75, 75, 75, 75, 75, 75, 75, 75}));

    ASSERT_EQ(findings.size(), 1u);
    EXPECT_EQ(findings[0].timestamp, t0 + 3s);
    EXPECT_TRUE(contains(findings[0].description, "for 10.0 s"))
        << findings[0].description;
    EXPECT_TRUE(contains(findings[0].description, "average 75.0%"))
        << findings[0].description;
}

// Checks that one low interval resets the sustained duration
TEST(AnalyzerCpuSustained, ResetsEpisodeAfterLowInterval)
{
    EXPECT_TRUE(Analyzer(cpu_config())
                    .analyze(cpu_series({75, 75, 25, 75, 75}))
                    .empty());
}

// Checks that separate episodes produce separate findings
TEST(AnalyzerCpuSustained, ReportsSeparateEpisodesSeparately)
{
    const auto findings = Analyzer(cpu_config()).analyze(
        cpu_series({75, 75, 75, 25, 75, 75, 75}));

    ASSERT_EQ(findings.size(), 2u);
    EXPECT_EQ(findings[0].timestamp, t0 + 3s);
    EXPECT_EQ(findings[1].timestamp, t0 + 7s);
}

// Checks that a zero sustained duration reports a single high interval
TEST(AnalyzerCpuSustained, ReportsSingleIntervalWhenSustainedDurationIsZero)
{
    Config config = cpu_config();
    config.cpu_sustained_duration = 0ms;

    const auto findings = Analyzer(config).analyze(cpu_series({25, 75, 25}));

    ASSERT_EQ(findings.size(), 1u);
    EXPECT_EQ(findings[0].timestamp, t0 + 2s);
    EXPECT_TRUE(contains(findings[0].description, "for 1.0 s"))
        << findings[0].description;
}

// CPU warm-up

// Checks that high usage only during the warm-up is not reported
TEST(AnalyzerCpuWarmup, IgnoresHighUsageDuringWarmup)
{
    Config config = cpu_config();
    config.warmup_duration = 3s;
    config.cpu_sustained_duration = 0ms;

    EXPECT_TRUE(
        Analyzer(config).analyze(cpu_series({75, 75, 75, 25, 25})).empty());
}

// Checks that time during the warm-up does not count towards the
// sustained duration
TEST(AnalyzerCpuWarmup, MeasuresSustainedDurationFromWarmupEnd)
{
    Config config = cpu_config();
    config.warmup_duration = 2s;

    // Usage is high for 4 s, but only 2 s of it are after the warm-up
    EXPECT_TRUE(
        Analyzer(config).analyze(cpu_series({75, 75, 75, 75})).empty());

    // With 4 s after the warm-up, the episode is confirmed at t0 + 5 s
    const auto findings =
        Analyzer(config).analyze(cpu_series({75, 75, 75, 75, 75, 75}));

    ASSERT_EQ(findings.size(), 1u);
    EXPECT_EQ(findings[0].timestamp, t0 + 5s);
    EXPECT_TRUE(contains(findings[0].description, "for 4.0 s"))
        << findings[0].description;
}

// Checks that only the part of an interval after the warm-up end counts
TEST(AnalyzerCpuWarmup, CountsOnlyPartOfIntervalAfterWarmup)
{
    Config config = cpu_config();
    config.warmup_duration = 1500ms;

    // The interval from 1 s to 2 s contributes 0.5 s, so the episode reaches
    // 3 s at t0 + 4.5 s and is confirmed by the sample at t0 + 5 s. The
    // average stays 75% because the CPU time is prorated as well
    const auto findings =
        Analyzer(config).analyze(cpu_series({75, 75, 75, 75, 75}));

    ASSERT_EQ(findings.size(), 1u);
    EXPECT_EQ(findings[0].timestamp, t0 + 5s);
    EXPECT_TRUE(
        contains(findings[0].description, "for 3.5 s (average 75.0%)"))
        << findings[0].description;
}

// Checks that an interval crossing the warm-up end with low usage does not
// start an episode
TEST(AnalyzerCpuWarmup, IgnoresLowUsageIntervalCrossingWarmupEnd)
{
    Config config = cpu_config();
    config.warmup_duration = 1500ms;
    config.cpu_sustained_duration = 0ms;

    const auto findings = Analyzer(config).analyze(cpu_series({75, 25, 75}));

    ASSERT_EQ(findings.size(), 1u);
    EXPECT_EQ(findings[0].timestamp, t0 + 3s);
}

// Combined

// Checks that memory and CPU findings are returned in time order
TEST(AnalyzerCombined, ReturnsFindingsOrderedByTimestamp)
{
    Config config = cpu_config();
    config.memory_growth_threshold_percent = 20.0;

    const std::vector<Sample> samples{
        make_sample(0s, 100, 0.0),
        make_sample(1s, 150, 0.75),
        make_sample(2s, 150, 1.5),
        make_sample(3s, 150, 2.25)};

    const auto findings = Analyzer(config).analyze(samples);

    ASSERT_EQ(findings.size(), 2u);
    EXPECT_EQ(findings[0].type, procwatch::memory_growth_finding);
    EXPECT_EQ(findings[0].timestamp, t0 + 1s);
    EXPECT_EQ(findings[1].type, procwatch::sustained_cpu_finding);
    EXPECT_EQ(findings[1].timestamp, t0 + 3s);
}

// Checks that a process within all limits produces no findings
TEST(AnalyzerCombined, ReportsNothingForStableProcess)
{
    EXPECT_TRUE(Analyzer(Config{})
                    .analyze(cpu_series({25, 25, 25, 25, 25, 25, 25, 25}))
                    .empty());
}

// Checks that analyzing the same samples twice gives the same result
TEST(AnalyzerCombined, IsStateless)
{
    const Analyzer analyzer(cpu_config());
    const auto samples = cpu_series({75, 75, 75, 25, 75, 75, 75});

    const auto first = analyzer.analyze(samples);
    const auto second = analyzer.analyze(samples);

    ASSERT_EQ(first.size(), second.size());

    for (std::size_t i = 0; i < first.size(); ++i)
    {
        EXPECT_EQ(first[i].type, second[i].type);
        EXPECT_EQ(first[i].description, second[i].description);
        EXPECT_EQ(first[i].timestamp, second[i].timestamp);
    }
}

}