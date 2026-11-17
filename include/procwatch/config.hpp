
#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace procwatch
{

// Holds the settings for one monitoring session
struct Config
{
    // Program to monitor, followed by its command-line arguments
    std::vector<std::string> command;

    // How often process metrics are collected
    std::chrono::milliseconds sampling_interval{1000};

    // Maximum time to monitor the target process
    std::chrono::milliseconds max_duration{60000};

    // Ignore memory growth during the initial start-up phase
    std::chrono::milliseconds warmup_duration{5000};

    // How long CPU usage must stay above the threshold to trigger a finding
    std::chrono::milliseconds cpu_sustained_duration{10000};

    // Maximum allowed memory growth compared to the baseline, in percent
    double memory_growth_threshold_percent{20.0};

    // CPU usage threshold as a percentage of one logical CPU
    // Values above 100% are possible for multi-threaded processes
    double cpu_usage_threshold_percent{90.0};

    // Where the JSON report is saved
    std::filesystem::path report_path{"procwatch-report.json"};
};

} // namespace procwatch
