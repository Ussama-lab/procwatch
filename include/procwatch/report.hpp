#pragma once

#include "procwatch/config.hpp"
#include "procwatch/types.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <stdexcept>
#include <string>

namespace procwatch
{

// Version of the JSON report format
inline constexpr int report_version = 1;

// Builds the JSON report for one monitoring session.
// Throws std::invalid_argument if the report data is inconsistent
nlohmann::json build_report(const ReportData& data, const Config& config);

// Writes the report as indented JSON, replacing an existing file.
// The report is written to a temporary file first and then renamed, so an
// interrupted write never leaves a partial report behind.
// Throws std::runtime_error if the file cannot be written
void write_report(
    const nlohmann::json& report,
    const std::filesystem::path& path
);

// Returns a short human-readable summary for the console.
// Throws std::invalid_argument if the report data is inconsistent
std::string format_summary(const ReportData& data);

}