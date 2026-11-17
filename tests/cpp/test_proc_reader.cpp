#include "procwatch/proc_reader.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>

#include <unistd.h>

namespace
{

using procwatch::ProcReader;
using procwatch::ReadStatus;

// Builds a realistic /proc/<pid>/stat line for parser tests
std::string stat_line(
    const std::string& name,
    char state = 'S',
    std::uint64_t utime = 250,
    std::uint64_t stime = 50)
{
    return "1234 (" + name + ") " + std::string(1, state) +
           " 1 1234 1234 0 -1 4194304 100 0 0 0 " +
           std::to_string(utime) + " " +
           std::to_string(stime) +
           " 0 0 20 0 1 0 12345 1000000 200\n";
}

// Provides realistic process status data with an RSS value of 2048 kB
const std::string status_content =
    "Name:\tmyprog\n"
    "VmPeak:\t    9000 kB\n"
    "VmHWM:\t    3000 kB\n"
    "VmRSS:\t    2048 kB\n"
    "Threads:\t1\n";

// ---------- parse_stat ----------

// Checks whether the parser correctly extracts the process state,
// user CPU time and system CPU time from /proc/<pid>/stat
TEST(ParseStat, ReadsStateAndCpuTicks)
{
    const auto fields = procwatch::parse_stat(stat_line("myprog", 'R'));

    ASSERT_TRUE(fields.has_value());
    EXPECT_EQ(fields->state, 'R');
    EXPECT_EQ(fields->utime_ticks, 250u);
    EXPECT_EQ(fields->stime_ticks, 50u);
}

// Checks that spaces and closing parentheses inside the process name
// do not interfere with parsing the remaining stat fields
TEST(ParseStat, HandlesSpacesAndParenthesesInProcessName)
{
    const auto fields = procwatch::parse_stat(
        stat_line("my) (program) 1 2 3"));

    ASSERT_TRUE(fields.has_value());
    EXPECT_EQ(fields->state, 'S');
    EXPECT_EQ(fields->utime_ticks, 250u);
    EXPECT_EQ(fields->stime_ticks, 50u);
}

// Checks that empty, truncated and invalid stat data is rejected,
// including non-numeric and negative CPU time values
TEST(ParseStat, RejectsMalformedContent)
{
    EXPECT_FALSE(procwatch::parse_stat("").has_value());

    EXPECT_FALSE(
        procwatch::parse_stat("1234 myprog S 1 2 3").has_value());

    // Truncated before stime
    EXPECT_FALSE(
        procwatch::parse_stat(
            "1234 (myprog) S 1 1234 1234 0 -1 0 0 0 0 0 250"
        ).has_value());

    // Non-numeric utime
    EXPECT_FALSE(
        procwatch::parse_stat(
            "1234 (myprog) S 1 1234 1234 0 -1 0 0 0 0 0 abc 50"
        ).has_value());

    // Negative utime
    EXPECT_FALSE(
        procwatch::parse_stat(
            "1234 (myprog) S 1 1234 1234 0 -1 0 0 0 0 0 -5 50"
        ).has_value());
}

// parse_rss_bytes

// Checks that VmRSS is correctly converted from kilobytes to bytes
TEST(ParseRssBytes, ConvertsKilobytesToBytes)
{
    const auto rss = procwatch::parse_rss_bytes(status_content);

    ASSERT_TRUE(rss.has_value());
    EXPECT_EQ(*rss, 2048u * 1024u);
}

// Checks that parsing fails when the VmRSS entry is missing
TEST(ParseRssBytes, ReturnsEmptyWhenLineIsMissing)
{
    EXPECT_FALSE(procwatch::parse_rss_bytes("").has_value());

    EXPECT_FALSE(
        procwatch::parse_rss_bytes(
            "Name:\tmyprog\nThreads:\t1\n"
        ).has_value());
}

// Checks that invalid RSS values, missing units and unsupported units
// are rejected instead of being interpreted as valid memory measurements
TEST(ParseRssBytes, RejectsMalformedLine)
{
    EXPECT_FALSE(
        procwatch::parse_rss_bytes("VmRSS:\t abc kB\n").has_value());

    EXPECT_FALSE(
        procwatch::parse_rss_bytes("VmRSS:\t 2048\n").has_value());

    EXPECT_FALSE(
        procwatch::parse_rss_bytes("VmRSS:\t 2048 MB\n").has_value());

    EXPECT_FALSE(
        procwatch::parse_rss_bytes("VmRSS:\t -1 kB\n").has_value());
}

// Checks that converting an extremely large RSS value does not
// cause an integer overflow when converting kilobytes to bytes
TEST(ParseRssBytes, RejectsOverflow)
{
    const std::string content =
        "VmRSS:\t" +
        std::to_string(std::numeric_limits<std::uint64_t>::max()) +
        " kB\n";

    EXPECT_FALSE(
        procwatch::parse_rss_bytes(content).has_value());
}

//ticks_to_seconds

// Checks that CPU clock ticks are converted to seconds
// using the supplied system tick rate
TEST(TicksToSeconds, DividesByTickRate)
{
    EXPECT_DOUBLE_EQ(
        procwatch::ticks_to_seconds(0, 100), 0.0);

    EXPECT_DOUBLE_EQ(
        procwatch::ticks_to_seconds(250, 100), 2.5);

    EXPECT_DOUBLE_EQ(
        procwatch::ticks_to_seconds(250, 1000), 0.25);
}

// Checks that invalid tick rates are rejected because
// division by zero or a negative rate would produce invalid results
TEST(TicksToSeconds, RejectsNonPositiveTickRate)
{
    EXPECT_THROW(
        procwatch::ticks_to_seconds(100, 0),
        std::invalid_argument);

    EXPECT_THROW(
        procwatch::ticks_to_seconds(100, -1),
        std::invalid_argument);
}

// ProcReader with a fake /proc directory

// Creates an isolated fake /proc directory for each test
// This allows process reading to be tested without depending on
// real processes or changing the actual Linux /proc filesystem
class ProcReaderTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const auto* info =
            ::testing::UnitTest::GetInstance()->current_test_info();

        root_ = std::filesystem::temp_directory_path() /
                ("procwatch_" + std::to_string(::getpid()) +
                 "_" + info->name());

        std::filesystem::remove_all(root_);

        std::filesystem::create_directories(
            root_ / std::to_string(pid_));
    }

    // Removes the temporary process directory after each test
    void TearDown() override
    {
        std::filesystem::remove_all(root_);
    }

    // Writes test data into the fake process directory
    void write_file(
        const std::string& name,
        const std::string& content) const
    {
        std::ofstream file(
            root_ / std::to_string(pid_) / name);

        file << content;
    }

    const pid_t pid_{1234};
    std::filesystem::path root_;
};

// Checks that ProcReader successfully reads RSS and CPU metrics
// from the fake stat and status files
TEST_F(ProcReaderTest, ReadsMetricsOfRunningProcess)
{
    write_file("stat", stat_line("myprog"));
    write_file("status", status_content);

    const auto result = ProcReader(root_, 100).read(pid_);

    ASSERT_EQ(result.status, ReadStatus::Ok);
    EXPECT_EQ(result.metrics.rss_bytes, 2048u * 1024u);
    EXPECT_EQ(result.metrics.cpu_time_ticks, 300u);
    EXPECT_DOUBLE_EQ(result.metrics.cpu_time_seconds, 3.0);
    EXPECT_TRUE(result.error.empty());
}

// Checks that a missing process directory is reported
// as a process that no longer exists
TEST_F(ProcReaderTest, ReportsProcessGoneWhenDirectoryIsMissing)
{
    const auto result = ProcReader(root_, 100).read(pid_ + 1);

    EXPECT_EQ(result.status, ReadStatus::ProcessGone);
}

// Checks that zombie processes are recognized as terminated
// before attempting to read their memory information
TEST_F(ProcReaderTest, ReportsProcessGoneForZombie)
{
    write_file("stat", stat_line("myprog", 'Z'));
    write_file(
        "status",
        "Name:\tmyprog\nState:\tZ (zombie)\n");

    const auto result = ProcReader(root_, 100).read(pid_);

    EXPECT_EQ(result.status, ReadStatus::ProcessGone);
}

// Checks that a missing status file is handled as a vanished process
// when the stat file was successfully read first
TEST_F(ProcReaderTest, ReportsProcessGoneWhenStatusFileIsMissing)
{
    write_file("stat", stat_line("myprog"));

    const auto result = ProcReader(root_, 100).read(pid_);

    EXPECT_EQ(result.status, ReadStatus::ProcessGone);
}

// Checks that invalid stat content produces a parsing error
// instead of returning incomplete process metrics
TEST_F(ProcReaderTest, ReportsParseErrorForMalformedStat)
{
    write_file("stat", "not a stat line\n");
    write_file("status", status_content);

    const auto result = ProcReader(root_, 100).read(pid_);

    EXPECT_EQ(result.status, ReadStatus::ParseError);
    EXPECT_FALSE(result.error.empty());
}

// Checks that a running process without a VmRSS entry
// produces a parsing error
TEST_F(ProcReaderTest, ReportsParseErrorWhenRunningProcessHasNoRss)
{
    write_file("stat", stat_line("myprog"));
    write_file("status", "Name:\tmyprog\n");

    const auto result = ProcReader(root_, 100).read(pid_);

    EXPECT_EQ(result.status, ReadStatus::ParseError);
    EXPECT_FALSE(result.error.empty());
}

// Checks that file access errors are reported correctly
// Reading a directory as a regular file fails on Linux
TEST_F(ProcReaderTest, ReportsReadErrorWhenFileCannotBeRead)
{
    std::filesystem::create_directory(
        root_ / std::to_string(pid_) / "stat");

    const auto result = ProcReader(root_, 100).read(pid_);

    EXPECT_EQ(result.status, ReadStatus::ReadError);
    EXPECT_FALSE(result.error.empty());
}

// Checks that PID zero is rejected as an invalid process identifier
TEST_F(ProcReaderTest, RejectsZeroPid)
{
    const auto result = ProcReader(root_, 100).read(0);

    EXPECT_EQ(result.status, ReadStatus::ReadError);
}

// Checks that negative PIDs are rejected as invalid input
TEST_F(ProcReaderTest, RejectsNegativePid)
{
    const auto result = ProcReader(root_, 100).read(-1);

    EXPECT_EQ(result.status, ReadStatus::ReadError);
}

// Checks that adding user and system CPU ticks cannot overflow
// the uint64_t range without producing a parsing error
TEST_F(ProcReaderTest, ReportsParseErrorWhenCpuTimeOverflows)
{
    write_file(
        "stat",
        stat_line(
            "myprog",
            'S',
            std::numeric_limits<std::uint64_t>::max(),
            1));

    write_file("status", status_content);

    const auto result = ProcReader(root_, 100).read(pid_);

    EXPECT_EQ(result.status, ReadStatus::ParseError);
    EXPECT_FALSE(result.error.empty());
}

// ProcReader construction and the real /proc

// Checks that ProcReader rejects zero and negative system tick rates
// during construction
TEST(ProcReaderConstruction, RejectsNonPositiveTickRate)
{
    EXPECT_THROW(ProcReader("/proc", 0), std::invalid_argument);
    EXPECT_THROW(ProcReader("/proc", -1), std::invalid_argument);
}

// Checks that ProcReader can read real metrics for the current process
// from the Linux /proc filesystem
TEST(ProcReaderLive, ReadsOwnProcess)
{
    const auto result = ProcReader().read(::getpid());

    ASSERT_EQ(result.status, ReadStatus::Ok) << result.error;

    EXPECT_GT(result.metrics.rss_bytes, 0u);

    EXPECT_DOUBLE_EQ(
        result.metrics.cpu_time_seconds,
        procwatch::ticks_to_seconds(
            result.metrics.cpu_time_ticks,
            procwatch::system_ticks_per_second()));
}

}
