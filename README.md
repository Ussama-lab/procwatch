# procwatch

**A lightweight Linux process stability monitor written in C++17.**

`procwatch` launches a program as a child process, samples its memory and CPU usage from `/proc`, and checks for memory growth, sustained high CPU usage, and abnormal termination. It prints a short console summary, writes a JSON report, and returns an exit code that scripts and CI jobs can act on.

It is a standalone project created for the MBition GmbH C++/Python take-home assignment; it is not a reconstruction of an internal MBition tool.

## Contents

- [Features](#features)
- [Scope](#scope)
- [Requirements](#requirements)
- [Build](#build)
- [Usage](#usage)
- [Options](#options)
- [How Monitoring Works](#how-monitoring-works)
- [Output](#output)
- [Exit Codes](#exit-codes)
- [Testing](#testing)
- [Project Structure](#project-structure)
- [Development Environment](#development-environment)
- [Known Limitations](#known-limitations)
- [License](#license)

## Features

- Launches one target command as a direct child process (`fork` and `execvp`).
- Samples resident memory (`VmRSS` from `/proc/<pid>/status`) and CPU time (`utime + stime` from `/proc/<pid>/stat`) at a fixed interval.
- Detects RSS growth above a configurable percentage of a baseline taken after a warm-up period.
- Detects CPU usage that stays above a configurable threshold for a configurable time. Values above 100% are supported for multi-threaded programs.
- Detects abnormal termination: a non-zero exit code or termination by a signal.
- Stops monitoring when the target exits, when the maximum duration is reached, or when procwatch receives `SIGINT` (Ctrl+C) or `SIGTERM`.
- Writes a JSON report and prints a human-readable summary.
- Returns documented exit codes.

RSS growth is reported as an observable indicator, not as proof of a memory leak.

## Scope

procwatch monitors exactly **one directly launched child process**. Processes started by the target are not monitored, and their CPU time is not included.

Not supported:

- Attaching to a process that is already running.
- Monitoring multiple processes or a whole process tree.
- Live dashboards, a graphical interface, or remote monitoring.
- Operating systems other than Linux.

## Requirements

procwatch runs on **Linux** only, because it relies on `/proc` and Linux process-control interfaces.

| Tool | Required | Verified with |
|---|---|---|
| C++17 compiler | Yes | GCC 13.3 |
| CMake | 3.24 or newer | 3.28.3 |
| Ninja | Recommended generator | 1.11.1 |
| Python 3 | Required by the CMake configuration | 3.12.3 |
| pytest | For the Python tests | 7.4.4 |

The first CMake configuration downloads GoogleTest 1.15.2 and nlohmann/json 3.11.3, so it needs network access.

The [development container](#development-environment) provides all of these tools.

## Build

From the repository root:

```bash
cmake -S . -B build -G Ninja
```

```bash
cmake --build build
```

This creates `build/procwatch` and the unit test binary `build/procwatch_tests`. Use the `build` directory, because the Python tests look for `build/procwatch`.

## Usage

```text
procwatch [options] -- <command> [args...]
```

The `--` separator is required. Everything after it is passed to the target unchanged, and the command is searched in `PATH`. The target inherits procwatch's standard input, output and error, and stays in the same process group.

Monitor a program with the default settings (up to 60 seconds):

```bash
./build/procwatch -- ./my_app --port 8080
```

Use a shorter run with an earlier analysis:

```bash
./build/procwatch --max-duration-ms 10000 --warmup-duration-ms 1000 --cpu-sustained-duration-ms 3000 -- sleep 2
```

```text
Process exited with code 0 after 2.0 s
Samples: 2
Findings: 0
Verdict: stable
Report: procwatch-report.json
```

A non-zero exit code is reported as an abnormal termination:

```bash
./build/procwatch --report-path exit.json -- sh -c "sleep 1; exit 3"
```

```text
Process exited with code 3 after 1.0 s
Samples: 2
Findings: 1
  [1.0 s] abnormal_termination: Process exited with code 3
Verdict: unstable
Report: exit.json
```

Invalid usage is rejected before anything is launched:

```text
$ ./build/procwatch sleep 1
procwatch: error: unexpected argument 'sleep' (use '--' before the command)
Try 'procwatch --help' for more information.
```

Run `./build/procwatch --help` for the full option list.

## Options

| Option | Unit | Default | Allowed values |
|---|---|---|---|
| `--sampling-interval-ms <n>` | ms | `1000` | 100 to 2,592,000,000 |
| `--max-duration-ms <n>` | ms | `60000` | 100 to 2,592,000,000 |
| `--warmup-duration-ms <n>` | ms | `5000` | 0 to 2,592,000,000 |
| `--cpu-sustained-duration-ms <n>` | ms | `10000` | 0 to 2,592,000,000 |
| `--memory-growth-threshold-percent <x>` | % | `20` | finite, ≥ 0 |
| `--cpu-usage-threshold-percent <x>` | % of one logical CPU | `90` | finite, ≥ 0; above 100 is valid |
| `--report-path <path>` | path | `procwatch-report.json` | see the validation rules below |
| `-h`, `--help` | | | Prints usage and exits with code 0 |

### Validation rules

Invalid values exit with code 2 before the target is started.

- Options accept `--name value` and `--name=value`. Unknown, abbreviated, repeated or valueless options are rejected.
- Durations are whole decimal numbers of milliseconds, so `1.5`, `10s`, `+1` and `-1` are rejected.
- Percentages are plain decimal numbers such as `20` or `12.5`. Exponents, `inf`, `nan` and negative values are rejected.
- The report path must name a file, must not be a directory, and its parent directory must exist and be writable.
- The monitoring window must leave room for both checks:

  ```text
  warmup + sampling interval + max(sampling interval, CPU sustained duration) <= max duration
  ```

  With the defaults this means at least 16,000 ms, so `--max-duration-ms 10000` alone is rejected. The shortest possible maximum duration is 200 ms.

## How Monitoring Works

**Sampling.** The first sample is taken right after the target has started. Later samples follow at the sampling interval, without accumulating small delays. The maximum duration is measured from the first sample, and a sample due exactly at the deadline is still taken.

**Warm-up.** The warm-up starts at the first sample and applies to both checks.

**Memory growth.** The baseline is the first sample at or after the end of the warm-up with a non-zero RSS. It stays fixed for the whole run. A finding is reported when RSS grows by more than the threshold above the baseline. A run produces at most one memory finding, timestamped at the first crossing.

**Sustained CPU usage.** For each interval between two samples:

```text
CPU usage % = (CPU time difference / elapsed wall-clock time) * 100
```

100% equals one fully busy logical CPU. Usage must stay above the threshold, without interruption, for at least the sustained duration. A single interval at or below the threshold ends the episode. Only time after the warm-up counts, and an interval that crosses the end of the warm-up counts only for its part after the warm-up. Each continuous episode produces one finding.

**Abnormal termination.** A non-zero exit code or termination by a signal produces an `abnormal_termination` finding.

**Stopping.** When the maximum duration is reached or procwatch receives `SIGINT` or `SIGTERM`, procwatch sends `SIGTERM` to the target. If the target is still running 2 seconds later, procwatch sends `SIGKILL`. A target that ends after a stop request has been observed counts as **stopped by procwatch**, which is not a finding. The exception is a crash signal such as `SIGSEGV` during shutdown, which is still reported as abnormal.

**Ctrl+C.** The terminal sends `SIGINT` to procwatch and the target at the same time. If the target ends from that signal before procwatch has observed the stop request, it is reported as an abnormal termination and procwatch exits with code 1.

**Read errors.** A failed read from `/proc` is skipped, because it can happen briefly while a process exits. If reads keep failing for 5 seconds while the target is still running, procwatch stops the target and exits with code 3.

## Output

### Console summary

The summary is printed to standard output after the target has ended, followed by the report path. Errors go to standard error.

### JSON report

The report is written to `--report-path`. It goes to a temporary file in the same directory first and is then renamed, so an interrupted write never leaves a partial report. Invalid UTF-8 in command arguments is replaced instead of failing the report. Keys appear in alphabetical order.

Example from the stable run above:

```json
{
  "config": {
    "command": ["sleep", "2"],
    "cpu_sustained_duration_ms": 3000,
    "cpu_usage_threshold_percent": 90.0,
    "max_duration_ms": 10000,
    "memory_growth_threshold_percent": 20.0,
    "sampling_interval_ms": 1000,
    "warmup_duration_ms": 1000
  },
  "duration_seconds": 2.001803834,
  "findings": [],
  "process": {
    "exit_code": 0,
    "signal": null,
    "termination": "exited"
  },
  "report_version": 1,
  "samples": {
    "count": 2,
    "cpu_time_seconds": 0.0,
    "first_offset_seconds": 0.000615291,
    "last_offset_seconds": 1.002037542,
    "rss_bytes": {
      "first": 520192,
      "last": 1306624,
      "peak": 1306624
    }
  },
  "stable": true,
  "started_at": "2026-11-19T21:33:34.359Z"
}
```

| Field | Meaning |
|---|---|
| `report_version` | Version of the report format, currently `1` |
| `started_at` | Start of monitoring in UTC, ISO 8601 |
| `duration_seconds` | Length of the monitoring session |
| `config` | The configuration that was used |
| `process.termination` | `exited`, `signaled` or `stopped_by_procwatch` |
| `process.exit_code` / `process.signal` | Set for `exited` and `signaled` respectively, otherwise `null` |
| `samples` | Sample count, first and last sample time, RSS (first, last, peak) and cumulative CPU time; values are `null` when no sample was taken |
| `findings` | `type`, `description` and `offset_seconds` for each finding, in time order |
| `stable` | `true` if there are no findings |

Finding types are `memory_growth`, `sustained_cpu` and `abnormal_termination`. All offsets are seconds since the start of monitoring.

## Exit Codes

| Code | Meaning | Report written |
|---|---|---|
| `0` | No stability issue detected, including a target stopped at the maximum duration or by a signal to procwatch | Yes |
| `1` | One or more findings | Yes |
| `2` | Invalid usage or configuration, or the command could not be executed | No |
| `3` | Internal error, such as a failed system call, persistent read errors or a failed report write | Not guaranteed |

The target's own exit code is not passed through. It appears in the summary and the report.

## Testing

Build the project first, then run all tests:

```bash
ctest --test-dir build --output-on-failure
```

Run only the C++ unit tests:

```bash
./build/procwatch_tests
```

Run only the Python tests:

```bash
python3 -m pytest tests/python
```

CTest registers **109 entries**: the 108 GoogleTest cases, plus one entry named `python_component_tests` that runs the whole Python suite.

### C++ unit tests

| Component | File | Test cases |
|---|---|---|
| `ProcReader` (`/proc` parsing and reading) | `tests/cpp/test_proc_reader.cpp` | 21 |
| `Sampler` (sampling schedule) | `tests/cpp/test_sampler.cpp` | 15 |
| `Analyzer` (memory and CPU analysis) | `tests/cpp/test_analyzer.cpp` | 38 |
| Report (JSON, file writing, summary) | `tests/cpp/test_report.cpp` | 34 |
| **Total** | | **108** |

The unit tests use synthetic timestamps and a temporary fake `/proc` directory, so they don't depend on timing.

### Python command-line tests

`tests/python/test_cli.py` runs the built executable and contains 8 test cases:

| Test | Cases | What it checks |
|---|---|---|
| `test_help_prints_usage` | `--help`, `-h` | Exit code 0 and usage text on standard output |
| `test_valid_command_writes_stable_report` | `-- /bin/true` | A real child process is launched, procwatch exits with code 0, prints a stable summary, and writes a JSON report with the expected command, process result and verdict |
| `test_incomplete_command_line_is_rejected` | 5 incomplete command lines | Exit code 2, an error message, and no report file |

### Not yet covered by automated tests

- **Supervisor unit tests.** `tests/cpp/test_supervisor.cpp` is an empty placeholder, and the Supervisor is not compiled into the unit test binary.
- **Signals.** Stopping with Ctrl+C or `SIGTERM`, and a target that ends from a signal.
- **Timeouts.** Stopping at the maximum duration, including the `SIGTERM` to `SIGKILL` escalation.
- **Real stability violations.** Memory growth, sustained high CPU usage, and a non-zero exit code from a real target.
- **Launch and runtime errors.** A command that cannot be executed (exit code 2), and persistent `/proc` read failures (exit code 3).

`tests/python/test_json_report.py` and `tests/python/test_process_monitoring.py` are empty placeholders reserved for these end-to-end tests.

## Project Structure

```text
.
├── .devcontainer/          Ubuntu 24.04 development container
├── CMakeLists.txt          Build, dependencies and test registration
├── docs/
│   ├── requirements.md     Functional and non-functional requirements
│   └── diagrams/           System architecture diagram
├── include/procwatch/      Public headers
├── src/
│   ├── main.cpp            Command-line parsing, validation and exit codes
│   ├── supervisor.cpp      Process launch, sampling loop, signals, termination
│   ├── proc_reader.cpp     Reading and parsing /proc/<pid>/stat and status
│   ├── sampler.cpp         Timestamped samples at a fixed interval
│   ├── analyzer.cpp        Memory-growth and sustained-CPU detection
│   └── report.cpp          JSON report, file writing and console summary
├── tests/
│   ├── cpp/                GoogleTest unit tests
│   └── python/             pytest command-line tests
└── requirements-dev.txt    Python test dependency
```

The data flow is `main` → `Supervisor` → `ProcReader` and `Sampler` → `Analyzer` → back to `main`, which writes the report. Shared types and the configuration are in `include/procwatch/types.hpp` and `include/procwatch/config.hpp`.

## Development Environment

The repository includes an Ubuntu 24.04 development container in `.devcontainer/` with GCC, CMake, Ninja, GDB, Git, Python 3 and pytest.

1. Open the repository in Visual Studio Code.
2. Install the Dev Containers extension.
3. Choose **Reopen in Container**.

## Known Limitations

- **Tick granularity.** CPU time is counted in clock ticks (typically 10 ms), which is why the minimum sampling interval is 100 ms.
- **Warm-up of 0.** The first sample is taken right after the program starts, before its normal start-up allocations. A warm-up of 0 can therefore report memory growth for an ordinary program.
- **Ctrl+C race.** Because of the race described in [How Monitoring Works](#how-monitoring-works), an interrupted run can occasionally end with an abnormal finding and exit code 1.
- **Stop time.** A run can last up to about 2 seconds past the maximum duration while the target shuts down. If the target can't be killed, procwatch gives up after 5 more seconds and exits with code 3.
- **Ctrl+C while writing.** Pressing Ctrl+C while procwatch writes the report can end procwatch. The report is never left partially written, but a temporary file may remain.
- **Child processes.** Processes started by the target are neither monitored nor counted.

## License

No license has been selected for this repository yet. Until one is added, no permissions beyond those granted by applicable copyright law should be assumed.