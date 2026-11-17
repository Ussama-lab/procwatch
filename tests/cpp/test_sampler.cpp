#include "procwatch/sampler.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include <unistd.h>

namespace
{

using namespace std::chrono_literals;

using procwatch::ProcReader;
using procwatch::ReadStatus;
using procwatch::Sampler;

// Tick rate used by the fake /proc directory: 100 ticks are one second
constexpr long ticks_per_second = 100;

// A fixed, arbitrary start time. Using synthetic time points keeps the
// tests independent of the real clock.
const Sampler::Clock::time_point t0 = Sampler::Clock::time_point{} + 1h;

// Builds a /proc/<pid>/stat line with the given state and CPU ticks
std::string stat_line(char state, std::uint64_t utime, std::uint64_t stime)
{
    return "1234 (myprog) " + std::string(1, state) +
           " 1 1234 1234 0 -1 4194304 100 0 0 0 " +
           std::to_string(utime) + " " + std::to_string(stime) +
           " 0 0 20 0 1 0 12345 1000000 200\n";
}

// Builds a /proc/<pid>/status file with the given RSS in kB
std::string status_content(std::uint64_t rss_kb)
{
    return "Name:\tmyprog\n"
           "VmRSS:\t    " + std::to_string(rss_kb) + " kB\n"
           "Threads:\t1\n";
}

// Provides a temporary fake /proc directory for one process
class SamplerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const auto* info =
            ::testing::UnitTest::GetInstance()->current_test_info();

        root_ = std::filesystem::temp_directory_path() /
                ("procwatch_sampler_" + std::to_string(::getpid()) + "_" +
                 info->name());

        std::filesystem::remove_all(root_);
        std::filesystem::create_directories(process_dir());
    }

    void TearDown() override
    {
        std::filesystem::remove_all(root_);
    }

    std::filesystem::path process_dir() const
    {
        return root_ / std::to_string(pid_);
    }

    // Writes stat and status files describing a running process
    void write_process(std::uint64_t rss_kb,
                       std::uint64_t utime,
                       std::uint64_t stime) const
    {
        write_file("stat", stat_line('S', utime, stime));
        write_file("status", status_content(rss_kb));
    }

    void write_file(const std::string& name,
                    const std::string& content) const
    {
        std::ofstream file(process_dir() / name);
        file << content;
    }

    Sampler make_sampler(std::chrono::milliseconds interval = 1s) const
    {
        return Sampler(pid_, ProcReader(root_, ticks_per_second), interval);
    }

    const pid_t pid_{1234};
    std::filesystem::path root_;
};

// Construction

// Checks that a zero or negative interval is rejected
TEST_F(SamplerTest, RejectsNonPositiveInterval)
{
    EXPECT_THROW(make_sampler(0ms), std::invalid_argument);
    EXPECT_THROW(make_sampler(-1ms), std::invalid_argument);
}

// Checks that a new sampler has no samples and is due immediately
TEST_F(SamplerTest, StartsEmptyAndIsDueImmediately)
{
    const Sampler sampler = make_sampler();

    EXPECT_TRUE(sampler.samples().empty());
    EXPECT_LE(sampler.next_sample_time(), Sampler::Clock::now());
}

// Successful sampling

// Checks that a successful read is stored with the given timestamp
// and the metrics reported by ProcReader
TEST_F(SamplerTest, StoresSuccessfulSample)
{
    write_process(2048, 250, 50);
    Sampler sampler = make_sampler();

    const auto result = sampler.sample(t0);

    ASSERT_EQ(result.status, ReadStatus::Ok) << result.error;
    ASSERT_EQ(sampler.samples().size(), 1u);

    const auto& sample = sampler.samples().front();
    EXPECT_EQ(sample.timestamp, t0);
    EXPECT_EQ(sample.metrics.rss_bytes, 2048u * 1024u);
    EXPECT_EQ(sample.metrics.cpu_time_ticks, 300u);
    EXPECT_DOUBLE_EQ(sample.metrics.cpu_time_seconds, 3.0);
}

// Checks that several samples are stored in order with their own
// timestamps and the metrics that were current at each read
TEST_F(SamplerTest, StoresMultipleSamplesInOrder)
{
    Sampler sampler = make_sampler();

    write_process(1000, 100, 0);
    ASSERT_EQ(sampler.sample(t0).status, ReadStatus::Ok);

    write_process(2000, 150, 50);
    ASSERT_EQ(sampler.sample(t0 + 1s).status, ReadStatus::Ok);

    write_process(3000, 300, 100);
    ASSERT_EQ(sampler.sample(t0 + 2s).status, ReadStatus::Ok);

    const auto& samples = sampler.samples();
    ASSERT_EQ(samples.size(), 3u);

    EXPECT_EQ(samples[0].timestamp, t0);
    EXPECT_EQ(samples[1].timestamp, t0 + 1s);
    EXPECT_EQ(samples[2].timestamp, t0 + 2s);

    EXPECT_EQ(samples[0].metrics.rss_bytes, 1000u * 1024u);
    EXPECT_EQ(samples[1].metrics.rss_bytes, 2000u * 1024u);
    EXPECT_EQ(samples[2].metrics.rss_bytes, 3000u * 1024u);

    EXPECT_EQ(samples[0].metrics.cpu_time_ticks, 100u);
    EXPECT_EQ(samples[1].metrics.cpu_time_ticks, 200u);
    EXPECT_EQ(samples[2].metrics.cpu_time_ticks, 400u);
}

// Failed reads

// Checks that a missing process is reported and not stored
TEST_F(SamplerTest, ReportsProcessGoneWhenProcessIsMissing)
{
    std::filesystem::remove_all(process_dir());
    Sampler sampler = make_sampler();

    const auto result = sampler.sample(t0);

    EXPECT_EQ(result.status, ReadStatus::ProcessGone);
    EXPECT_TRUE(sampler.samples().empty());
}

// Checks that a zombie process is reported as gone and not stored
TEST_F(SamplerTest, ReportsProcessGoneForZombie)
{
    write_file("stat", stat_line('Z', 250, 50));
    write_file("status", "Name:\tmyprog\nState:\tZ (zombie)\n");
    Sampler sampler = make_sampler();

    const auto result = sampler.sample(t0);

    EXPECT_EQ(result.status, ReadStatus::ProcessGone);
    EXPECT_TRUE(sampler.samples().empty());
}

// Checks that earlier samples are kept when the process disappears
TEST_F(SamplerTest, KeepsEarlierSamplesWhenProcessDisappears)
{
    write_process(2048, 250, 50);
    Sampler sampler = make_sampler();
    ASSERT_EQ(sampler.sample(t0).status, ReadStatus::Ok);

    std::filesystem::remove_all(process_dir());
    const auto result = sampler.sample(t0 + 1s);

    EXPECT_EQ(result.status, ReadStatus::ProcessGone);
    ASSERT_EQ(sampler.samples().size(), 1u);
    EXPECT_EQ(sampler.samples().front().timestamp, t0);
}

// Checks that malformed proc data is reported as a parse error
// with a message and is not stored
TEST_F(SamplerTest, ReportsParseErrorAndStoresNothing)
{
    write_file("stat", "not a stat line\n");
    write_file("status", status_content(2048));
    Sampler sampler = make_sampler();

    const auto result = sampler.sample(t0);

    EXPECT_EQ(result.status, ReadStatus::ParseError);
    EXPECT_FALSE(result.error.empty());
    EXPECT_TRUE(sampler.samples().empty());
}

// Checks that an unreadable proc file is reported as a read error
// with a message and is not stored
TEST_F(SamplerTest, ReportsReadErrorAndStoresNothing)
{
    // Reading a directory fails with EISDIR, independent of user privileges
    std::filesystem::create_directory(process_dir() / "stat");
    Sampler sampler = make_sampler();

    const auto result = sampler.sample(t0);

    EXPECT_EQ(result.status, ReadStatus::ReadError);
    EXPECT_FALSE(result.error.empty());
    EXPECT_TRUE(sampler.samples().empty());
}

// Checks that sampling can continue after a temporary read failure
TEST_F(SamplerTest, ContinuesAfterFailedRead)
{
    Sampler sampler = make_sampler();

    write_file("stat", "not a stat line\n");
    ASSERT_EQ(sampler.sample(t0).status, ReadStatus::ParseError);

    write_process(2048, 250, 50);
    ASSERT_EQ(sampler.sample(t0 + 1s).status, ReadStatus::Ok);

    ASSERT_EQ(sampler.samples().size(), 1u);
    EXPECT_EQ(sampler.samples().front().timestamp, t0 + 1s);
}

// Scheduling

// Checks that the next sample is due one interval after the first one
TEST_F(SamplerTest, SchedulesNextSampleOneIntervalLater)
{
    write_process(2048, 250, 50);
    Sampler sampler = make_sampler(500ms);

    sampler.sample(t0);

    EXPECT_EQ(sampler.next_sample_time(), t0 + 500ms);
}

// Checks that a slightly late sample does not shift the schedule,
// so delays do not accumulate over a long run
TEST_F(SamplerTest, KeepsFixedRateWhenSampleIsSlightlyLate)
{
    write_process(2048, 250, 50);
    Sampler sampler = make_sampler(1s);

    sampler.sample(t0);
    sampler.sample(t0 + 1s + 200ms);

    EXPECT_EQ(sampler.next_sample_time(), t0 + 2s);
}

// Checks that missed intervals are skipped instead of causing a burst
TEST_F(SamplerTest, SkipsMissedIntervalsWhenFarBehind)
{
    write_process(2048, 250, 50);
    Sampler sampler = make_sampler(1s);

    sampler.sample(t0);
    sampler.sample(t0 + 3500ms);

    EXPECT_EQ(sampler.next_sample_time(), t0 + 4500ms);
}

// Checks that a failed read still advances the schedule
TEST_F(SamplerTest, AdvancesScheduleWhenReadFails)
{
    std::filesystem::remove_all(process_dir());
    Sampler sampler = make_sampler(1s);

    sampler.sample(t0);

    EXPECT_EQ(sampler.next_sample_time(), t0 + 1s);
}

// Real /proc

// Checks that the sampler can read the test process itself
TEST(SamplerLive, SamplesOwnProcess)
{
    Sampler sampler(::getpid(), ProcReader(), 1s);

    const auto before = Sampler::Clock::now();
    const auto result = sampler.sample();
    const auto after = Sampler::Clock::now();

    ASSERT_EQ(result.status, ReadStatus::Ok) << result.error;
    ASSERT_EQ(sampler.samples().size(), 1u);
    EXPECT_GT(sampler.samples().front().metrics.rss_bytes, 0u);
    EXPECT_GE(sampler.samples().front().timestamp, before);
    EXPECT_LE(sampler.samples().front().timestamp, after);
}

}