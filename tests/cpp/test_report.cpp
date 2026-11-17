#include "procwatch/report.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace
{

using namespace std::chrono_literals;

using nlohmann::json;
using procwatch::Config;
using procwatch::Finding;
using procwatch::ReportData;
using procwatch::Sample;
using procwatch::Termination;

constexpr std::uint64_t mib = 1024 * 1024;

// Fixed, arbitrary times that keep the tests independent of the clock
const std::chrono::steady_clock::time_point t0 =
    std::chrono::steady_clock::time_point{} + 1h;
const std::chrono::system_clock::time_point wall_start =
    std::chrono::system_clock::time_point{} + 10s + 123ms;

Sample make_sample(
    std::chrono::milliseconds offset,
    std::uint64_t rss_mib,
    double cpu_seconds)
{
    Sample sample;
    sample.timestamp = t0 + offset;
    sample.metrics.rss_bytes = rss_mib * mib;
    sample.metrics.cpu_time_seconds = cpu_seconds;
    sample.metrics.cpu_time_ticks =
        static_cast<std::uint64_t>(cpu_seconds * 100.0);
    return sample;
}

Finding make_finding(
    std::chrono::milliseconds offset,
    const std::string& type,
    const std::string& description)
{
    return Finding{type, description, t0 + offset};
}

// A stable 12-second session with three samples and a normal exit
ReportData make_data()
{
    ReportData data;
    data.start_time = t0;
    data.started_at = wall_start;
    data.end_time = t0 + 12s;
    data.samples = {
        make_sample(0s, 10, 0.0),
        make_sample(5s, 30, 1.5),
        make_sample(11s, 20, 3.25)};
    data.process_result.termination = Termination::Exited;
    data.process_result.exit_code = 0;
    data.stable = true;
    return data;
}

// The same session with two findings
ReportData make_unstable_data()
{
    ReportData data = make_data();
    data.findings = {
        make_finding(5s, "memory_growth", "RSS grew"),
        make_finding(11s, "sustained_cpu", "CPU stayed high")};
    data.stable = false;
    return data;
}

Config make_config()
{
    Config config;
    config.command = {"./my_app", "--flag"};
    return config;
}

bool contains(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

// Sets the process umask and restores the previous one when destroyed
class UmaskGuard
{
public:
    explicit UmaskGuard(mode_t mask)
        : previous_(::umask(mask))
    {
    }

    ~UmaskGuard()
    {
        ::umask(previous_);
    }

    UmaskGuard(const UmaskGuard&) = delete;
    UmaskGuard& operator=(const UmaskGuard&) = delete;

private:
    mode_t previous_;
};

// Expects build_report to reject the data
void expect_invalid(const ReportData& data)
{
    EXPECT_THROW(
        procwatch::build_report(data, make_config()), std::invalid_argument);
}

// build_report: content

// Checks the format version, start time and duration
TEST(BuildReport, ContainsVersionStartTimeAndDuration)
{
    const json report = procwatch::build_report(make_data(), make_config());

    EXPECT_EQ(report["report_version"], procwatch::report_version);
    EXPECT_EQ(report["started_at"], "1970-01-01T00:00:10.123Z");
    EXPECT_DOUBLE_EQ(report["duration_seconds"].get<double>(), 12.0);
}

// Checks that every configuration value is written with its unit
TEST(BuildReport, ContainsConfiguration)
{
    Config input = make_config();
    input.max_duration = 30s;

    const json config = procwatch::build_report(make_data(), input)["config"];

    EXPECT_EQ(config["command"], json::array({"./my_app", "--flag"}));
    EXPECT_EQ(config["sampling_interval_ms"], 1000);
    EXPECT_EQ(config["max_duration_ms"], 30000);
    EXPECT_EQ(config["warmup_duration_ms"], 5000);
    EXPECT_EQ(config["cpu_sustained_duration_ms"], 10000);
    EXPECT_DOUBLE_EQ(
        config["memory_growth_threshold_percent"].get<double>(), 20.0);
    EXPECT_DOUBLE_EQ(config["cpu_usage_threshold_percent"].get<double>(), 90.0);
}

// Checks a normal and a non-zero exit
TEST(BuildReport, DescribesExitedProcess)
{
    ReportData data = make_data();

    json process = procwatch::build_report(data, make_config())["process"];
    EXPECT_EQ(process["termination"], "exited");
    EXPECT_EQ(process["exit_code"], 0);
    EXPECT_TRUE(process["signal"].is_null());

    data.process_result.exit_code = 3;
    process = procwatch::build_report(data, make_config())["process"];
    EXPECT_EQ(process["exit_code"], 3);
}

// Checks a process killed by a signal
TEST(BuildReport, DescribesSignaledProcess)
{
    ReportData data = make_data();
    data.process_result.termination = Termination::Signaled;
    data.process_result.signal = 9;

    const json process = procwatch::build_report(data, make_config())["process"];

    EXPECT_EQ(process["termination"], "signaled");
    EXPECT_TRUE(process["exit_code"].is_null());
    EXPECT_EQ(process["signal"], 9);
}

// Checks a process ended by procwatch itself
TEST(BuildReport, DescribesProcessStoppedByProcwatch)
{
    ReportData data = make_data();
    data.process_result.termination = Termination::StoppedByProcwatch;

    const json process = procwatch::build_report(data, make_config())["process"];

    EXPECT_EQ(process["termination"], "stopped_by_procwatch");
    EXPECT_TRUE(process["exit_code"].is_null());
    EXPECT_TRUE(process["signal"].is_null());
}

// Checks the sample summary
TEST(BuildReport, SummarizesSamples)
{
    const json samples =
        procwatch::build_report(make_data(), make_config())["samples"];

    EXPECT_EQ(samples["count"], 3);
    EXPECT_DOUBLE_EQ(samples["first_offset_seconds"].get<double>(), 0.0);
    EXPECT_DOUBLE_EQ(samples["last_offset_seconds"].get<double>(), 11.0);
    EXPECT_EQ(samples["rss_bytes"]["first"], 10 * mib);
    EXPECT_EQ(samples["rss_bytes"]["last"], 20 * mib);
    EXPECT_EQ(samples["rss_bytes"]["peak"], 30 * mib);
    EXPECT_DOUBLE_EQ(samples["cpu_time_seconds"].get<double>(), 3.25);
}

// Checks that a session without samples still produces a complete summary
TEST(BuildReport, HandlesSessionWithoutSamples)
{
    ReportData data = make_data();
    data.samples.clear();

    const json samples = procwatch::build_report(data, make_config())["samples"];

    EXPECT_EQ(samples["count"], 0);
    EXPECT_TRUE(samples["first_offset_seconds"].is_null());
    EXPECT_TRUE(samples["last_offset_seconds"].is_null());
    EXPECT_TRUE(samples["rss_bytes"].is_null());
    EXPECT_TRUE(samples["cpu_time_seconds"].is_null());
}

// Checks findings, their offsets and their order
TEST(BuildReport, ListsFindingsInOrder)
{
    const json report =
        procwatch::build_report(make_unstable_data(), make_config());
    const json& findings = report["findings"];

    ASSERT_EQ(findings.size(), 2u);
    EXPECT_EQ(findings[0]["type"], "memory_growth");
    EXPECT_DOUBLE_EQ(findings[0]["offset_seconds"].get<double>(), 5.0);
    EXPECT_EQ(findings[0]["description"], "RSS grew");
    EXPECT_EQ(findings[1]["type"], "sustained_cpu");
    EXPECT_DOUBLE_EQ(findings[1]["offset_seconds"].get<double>(), 11.0);
}

// Checks the verdict for a stable and an unstable session
TEST(BuildReport, ContainsVerdict)
{
    const json stable = procwatch::build_report(make_data(), make_config());
    EXPECT_EQ(stable["stable"], true);
    EXPECT_TRUE(stable["findings"].is_array());
    EXPECT_TRUE(stable["findings"].empty());

    const json unstable =
        procwatch::build_report(make_unstable_data(), make_config());
    EXPECT_EQ(unstable["stable"], false);
}

// build_report: session and verdict validation

// Checks that the verdict must match the findings
TEST(BuildReportValidation, RejectsVerdictThatContradictsFindings)
{
    ReportData data = make_unstable_data();
    data.stable = true;
    expect_invalid(data);

    data = make_data();
    data.stable = false;
    expect_invalid(data);
}

// Checks that times outside the monitoring period are rejected
TEST(BuildReportValidation, RejectsTimesOutsideSession)
{
    ReportData data = make_data();
    data.end_time = t0 - 1s;
    expect_invalid(data);

    data = make_data();
    data.samples.front().timestamp = t0 - 1ms;
    expect_invalid(data);

    data = make_unstable_data();
    data.findings.back().timestamp = t0 + 13s;
    expect_invalid(data);
}

// build_report: sample validation

// Checks that duplicate and decreasing sample timestamps are rejected
TEST(BuildReportValidation, RejectsNonIncreasingSampleTimestamps)
{
    ReportData data = make_data();
    data.samples[1].timestamp = data.samples[0].timestamp;
    expect_invalid(data);

    data = make_data();
    data.samples[2].timestamp = t0 + 4s;
    expect_invalid(data);
}

// Checks that non-finite CPU times are rejected
TEST(BuildReportValidation, RejectsNonFiniteCpuTime)
{
    for (const double value : {std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::infinity()})
    {
        ReportData data = make_data();
        data.samples.back().metrics.cpu_time_seconds = value;
        expect_invalid(data);
    }
}

// Checks that negative CPU times are rejected
TEST(BuildReportValidation, RejectsNegativeCpuTime)
{
    ReportData data = make_data();
    data.samples.front().metrics.cpu_time_seconds = -0.5;
    expect_invalid(data);
}

// Checks that cumulative CPU time must not decrease, in seconds or ticks
TEST(BuildReportValidation, RejectsDecreasingCpuTime)
{
    ReportData data = make_data();
    data.samples[2].metrics.cpu_time_seconds = 1.0;
    expect_invalid(data);

    data = make_data();
    data.samples[2].metrics.cpu_time_ticks = 100;
    expect_invalid(data);
}

// Checks that unchanged CPU time between samples is accepted
TEST(BuildReportValidation, AcceptsUnchangedCpuTime)
{
    ReportData data = make_data();
    data.samples[2].metrics = data.samples[1].metrics;

    EXPECT_NO_THROW(procwatch::build_report(data, make_config()));
}

// build_report: finding validation

// Checks that findings may share a timestamp
TEST(BuildReportValidation, AcceptsFindingsWithSameTimestamp)
{
    ReportData data = make_unstable_data();
    data.findings[1].timestamp = data.findings[0].timestamp;

    EXPECT_NO_THROW(procwatch::build_report(data, make_config()));
}

// Checks that findings out of chronological order are rejected
TEST(BuildReportValidation, RejectsFindingsOutOfOrder)
{
    ReportData data = make_unstable_data();
    std::swap(data.findings[0], data.findings[1]);

    expect_invalid(data);
}

// build_report: termination validation

// Checks the exit code range and that an exited process has no signal
TEST(BuildReportValidation, ValidatesExitedProcess)
{
    ReportData data = make_data();

    data.process_result.exit_code = 255;
    EXPECT_NO_THROW(procwatch::build_report(data, make_config()));

    for (const int exit_code : {-1, 256})
    {
        data.process_result.exit_code = exit_code;
        expect_invalid(data);
    }

    data.process_result.exit_code = 1;
    data.process_result.signal = 9;
    expect_invalid(data);
}

// Checks the signal range and that a signaled process has no exit code
TEST(BuildReportValidation, ValidatesSignaledProcess)
{
    ReportData data = make_data();
    data.process_result.termination = Termination::Signaled;

    data.process_result.signal = NSIG - 1;
    EXPECT_NO_THROW(procwatch::build_report(data, make_config()));

    for (const int signal : {0, -1, NSIG})
    {
        data.process_result.signal = signal;
        expect_invalid(data);
    }

    data.process_result.signal = 9;
    data.process_result.exit_code = 1;
    expect_invalid(data);
}

// Checks that a process stopped by procwatch has no exit code or signal
TEST(BuildReportValidation, ValidatesProcessStoppedByProcwatch)
{
    ReportData data = make_data();
    data.process_result.termination = Termination::StoppedByProcwatch;

    data.process_result.exit_code = 1;
    expect_invalid(data);

    data.process_result.exit_code = 0;
    data.process_result.signal = 15;
    expect_invalid(data);
}

// write_report

// Provides an empty temporary directory for file tests
class WriteReportTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const auto* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        dir_ = std::filesystem::temp_directory_path() /
               ("procwatch_report_" + std::to_string(::getpid()) + "_" +
                info->name());
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
    }

    void TearDown() override
    {
        std::filesystem::permissions(dir_, std::filesystem::perms::owner_all);
        std::filesystem::remove_all(dir_);
    }

    static std::string read_file(const std::filesystem::path& path)
    {
        std::ifstream file(path);
        return {std::istreambuf_iterator<char>(file), {}};
    }

    // Names of all entries in the test directory, sorted
    std::vector<std::string> entries() const
    {
        std::vector<std::string> names;

        for (const auto& entry : std::filesystem::directory_iterator(dir_))
        {
            names.push_back(entry.path().filename().string());
        }

        std::sort(names.begin(), names.end());
        return names;
    }

    std::filesystem::path dir_;
};

// Checks that the written file contains the same JSON and that no
// temporary file is left behind
TEST_F(WriteReportTest, WritesReadableJson)
{
    const json report =
        procwatch::build_report(make_unstable_data(), make_config());
    const auto path = dir_ / "report.json";

    procwatch::write_report(report, path);

    EXPECT_EQ(json::parse(read_file(path)), report);
    EXPECT_EQ(entries(), std::vector<std::string>{"report.json"});
}

// Checks that an existing report is replaced
TEST_F(WriteReportTest, ReplacesExistingFile)
{
    const auto path = dir_ / "report.json";
    std::ofstream(path) << "old content";

    const json report = procwatch::build_report(make_data(), make_config());
    procwatch::write_report(report, path);

    EXPECT_EQ(json::parse(read_file(path)), report);
}

// Checks that the report gets the usual permissions for a new file
TEST_F(WriteReportTest, UsesPermissionsFromUmask)
{
    {
        const UmaskGuard umask_guard(022);
        procwatch::write_report(json::object(), dir_ / "report.json");
    }

    using std::filesystem::perms;
    const perms expected = perms::owner_read | perms::owner_write |
                           perms::group_read | perms::others_read;

    EXPECT_EQ(
        std::filesystem::status(dir_ / "report.json").permissions(), expected);
}

// Checks that an unrelated file with a temporary-looking name is left alone
TEST_F(WriteReportTest, LeavesOtherTemporaryFilesUntouched)
{
    const auto path = dir_ / "report.json";
    std::ofstream(dir_ / "report.json.tmp") << "keep";

    procwatch::write_report(json::object(), path);

    EXPECT_EQ(read_file(dir_ / "report.json.tmp"), "keep");
    EXPECT_EQ(
        entries(), (std::vector<std::string>{"report.json", "report.json.tmp"}));
}

// Checks that concurrent writes to the same path use separate temporary
// files, all succeed and leave one complete report behind
TEST_F(WriteReportTest, ConcurrentWritesDoNotInterfere)
{
    constexpr int writer_count = 8;
    const auto path = dir_ / "report.json";
    std::vector<std::thread> writers;
    std::vector<int> failures(writer_count, 0);

    for (int i = 0; i < writer_count; ++i)
    {
        writers.emplace_back([&path, &failures, i]()
        {
            try
            {
                procwatch::write_report(json{{"writer", i}}, path);
            }
            catch (const std::exception&)
            {
                failures[static_cast<std::size_t>(i)] = 1;
            }
        });
    }

    for (std::thread& writer : writers)
    {
        writer.join();
    }

    EXPECT_EQ(std::count(failures.begin(), failures.end(), 1), 0);

    const json written = json::parse(read_file(path));
    EXPECT_GE(written["writer"].get<int>(), 0);
    EXPECT_LT(written["writer"].get<int>(), writer_count);
    EXPECT_EQ(entries(), std::vector<std::string>{"report.json"});
}

// Checks that special characters survive writing and reading back, and
// that invalid UTF-8 is replaced instead of failing
TEST_F(WriteReportTest, KeepsSpecialCharacters)
{
    Config config = make_config();
    config.command = {
        "./app", "say \"hi\"\nnow", "gr\xC3\xBC\xC3\x9F", "bad\xFF"};

    ReportData data = make_unstable_data();
    data.findings[0].description = "tab\there \\ \"quoted\"";

    const auto path = dir_ / "report.json";
    procwatch::write_report(procwatch::build_report(data, config), path);

    const json written = json::parse(read_file(path));
    EXPECT_EQ(written["config"]["command"][1], "say \"hi\"\nnow");
    EXPECT_EQ(written["config"]["command"][2], "gr\xC3\xBC\xC3\x9F");
    EXPECT_EQ(written["config"]["command"][3], "bad\xEF\xBF\xBD");
    EXPECT_EQ(
        written["findings"][0]["description"], "tab\there \\ \"quoted\"");
}

// Checks that a missing directory is reported with the path
TEST_F(WriteReportTest, ThrowsForMissingDirectory)
{
    const auto path = dir_ / "missing" / "report.json";

    try
    {
        procwatch::write_report(json::object(), path);
        FAIL() << "Expected std::runtime_error";
    }
    catch (const std::runtime_error& error)
    {
        EXPECT_TRUE(contains(error.what(), path.string())) << error.what();
    }
}

// Checks that a failed rename removes the temporary file and keeps the
// directory at the target path
TEST_F(WriteReportTest, ThrowsWhenTargetIsDirectory)
{
    const auto path = dir_ / "report.json";
    std::filesystem::create_directory(path);

    EXPECT_THROW(
        procwatch::write_report(json::object(), path), std::runtime_error);
    EXPECT_TRUE(std::filesystem::is_directory(path));
    EXPECT_EQ(entries(), std::vector<std::string>{"report.json"});
}

// Checks that a directory without write permission is reported and that
// nothing is created in it
TEST_F(WriteReportTest, ThrowsForReadOnlyDirectory)
{
    if (::geteuid() == 0)
    {
        GTEST_SKIP() << "root ignores directory permissions";
    }

    std::filesystem::permissions(
        dir_,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);

    EXPECT_THROW(
        procwatch::write_report(json::object(), dir_ / "report.json"),
        std::runtime_error);

    std::filesystem::permissions(dir_, std::filesystem::perms::owner_all);
    EXPECT_TRUE(entries().empty());
}

// format_summary

// Checks the summary of a stable session
TEST(FormatSummary, DescribesStableSession)
{
    const std::string summary = procwatch::format_summary(make_data());

    EXPECT_TRUE(contains(summary, "Process exited with code 0 after 12.0 s"))
        << summary;
    EXPECT_TRUE(contains(summary, "Samples: 3")) << summary;
    EXPECT_TRUE(contains(summary, "Findings: 0")) << summary;
    EXPECT_TRUE(contains(summary, "Verdict: stable")) << summary;
}

// Checks that every finding and the verdict appear in the summary
TEST(FormatSummary, ListsFindings)
{
    const std::string summary = procwatch::format_summary(make_unstable_data());

    EXPECT_TRUE(contains(summary, "Findings: 2")) << summary;
    EXPECT_TRUE(contains(summary, "[5.0 s] memory_growth: RSS grew"))
        << summary;
    EXPECT_TRUE(contains(summary, "[11.0 s] sustained_cpu: CPU stayed high"))
        << summary;
    EXPECT_TRUE(contains(summary, "Verdict: unstable")) << summary;
}

// Checks the wording for the other termination kinds
TEST(FormatSummary, DescribesTermination)
{
    ReportData data = make_data();

    data.process_result.termination = Termination::Signaled;
    data.process_result.signal = 9;
    EXPECT_TRUE(
        contains(procwatch::format_summary(data), "killed by signal 9"));

    data.process_result.termination = Termination::StoppedByProcwatch;
    data.process_result.signal = 0;
    EXPECT_TRUE(
        contains(procwatch::format_summary(data), "stopped by procwatch"));
}

// Checks that the summary applies the same validation as the report
TEST(FormatSummary, RejectsInvalidData)
{
    ReportData data = make_data();
    data.process_result.signal = 9;

    EXPECT_THROW(procwatch::format_summary(data), std::invalid_argument);
}

}