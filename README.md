# procwatch

**A lightweight Linux process stability monitor written in C++.**

`procwatch` is a command-line tool for observing the runtime behaviour of a program on Linux. It launches a target program, samples selected resource-usage metrics, detects configurable stability conditions, and produces a concise console summary and a machine-readable JSON report.

The project is intended for developers who need a small, scriptable way to observe a process during a test or local run. It is a standalone project created for the MBition GmbH C++/Python take-home assignment; it is not a reconstruction of an internal MBition tool.

> **Project status:** Initial development. The v1 scope has been agreed, but the implementation, build instructions, and executable usage examples are not yet available. Features and interfaces described below are planned and should not be considered implemented.

## Contents

- [Goals](#goals)
- [Planned Features](#planned-features)
- [Version 1 Scope](#version-1-scope)
- [Non-Goals](#non-goals)
- [Platform and Prerequisites](#platform-and-prerequisites)
- [Build and Installation](#build-and-installation)
- [Usage](#usage)
- [Configuration](#configuration)
- [Reports and Exit Codes](#reports-and-exit-codes)
- [Testing](#testing)
- [Development Environment](#development-environment)
- [Project Documentation](#project-documentation)
- [Contributing](#contributing)
- [Support and Issue Reporting](#support-and-issue-reporting)
- [License](#license)
- [Documentation Quality Standards](#documentation-quality-standards)

## Goals

The initial release aims to provide a focused and understandable process-monitoring utility that:

- Makes selected runtime resource trends observable.
- Detects a small set of configurable stability conditions.
- Can be used from a terminal or an automated test/CI workflow.
- Produces reports that can be inspected by developers or processed by other tools.

The emphasis is on a small, pragmatic implementation with clear requirements, automated tests, and maintainable documentation.

## Planned Features

Version 1 is planned to:

- Launch a target command as a child process.
- Periodically read resident memory (RSS) from `/proc/<pid>/status`, using the `VmRSS` field.
- Read CPU time from `/proc/<pid>/stat`, using `utime` and `stime`, and derive CPU usage from successive samples.
- Detect RSS growth above a configurable percentage relative to a baseline established after a warm-up period.
- Detect CPU usage above a configurable threshold when it remains above that threshold for a specified period.
- Detect termination by signal and non-zero exit status.
- Stop monitoring when the configured duration expires or the target process exits.
- Handle `SIGINT` and `SIGTERM` and attempt to write a final report after an orderly stop.
- Generate a JSON report containing the run configuration, sample summary, timestamped findings, and overall verdict.
- Print a concise human-readable summary to standard output.

RSS growth is treated as an observable indicator, not proof of a memory leak. The tool will report memory growth without claiming to diagnose its underlying cause.

## Version 1 Scope

The command-line interface is planned around the following structure:

```text
procwatch [options] -- <command> [args...]
```

Version 1 monitors **one directly launched child process**. It does not monitor additional processes spawned by that process.

The monitor will periodically sample resource usage and evaluate configurable memory-growth and sustained-CPU thresholds. Monitoring will finish when the target process exits or the configured duration is reached.

## Non-Goals

The following capabilities are deliberately excluded from version 1:

- Attaching to an arbitrary process that is already running (for example, through a `--pid` option).
- Monitoring multiple processes or an entire process tree.
- Providing a live dashboard or graphical user interface.
- Remote or network-based monitoring and notifications.
- Correlating process metrics with DLT logs, Wireshark packet captures, CAN, or SocketCAN data.
- Supporting operating systems other than Linux.

These topics may be reconsidered for a future release, but they are not commitments for version 1.

## Platform and Prerequisites

Version 1 targets **Linux** and relies on the `/proc` process-information filesystem and Linux process-control interfaces. macOS and Windows are not supported as runtime platforms.

The development environment is based on Ubuntu 24.04 and currently includes:

- GCC/G++.
- CMake.
- Ninja.
- Python 3.
- pytest.
- Git.

The exact minimum supported compiler and tool versions, along with any runtime dependencies, will be documented once they have been established and verified by the build configuration.

## Build and Installation

Build and installation instructions will be added once the project has a working build configuration.

The intended build system is CMake. The development container provides the current toolchain used for implementation and testing.

No installable or runnable release is available yet.

## Usage

The command-line interface and its options have not been implemented yet.

The following example illustrates the planned command structure and is **not yet executable**:

```bash
procwatch [options] -- ./path/to/program [program arguments...]
```

A verified end-to-end example will be added after the executable and its argument handling have been implemented.

## Configuration

The version 1 design includes configurable values for:

- Monitoring duration.
- Sampling interval.
- Memory-growth threshold.
- Warm-up period.
- CPU-usage threshold.
- Sustained-CPU period.

Exact option names, units, default values, validation rules, and boundary behaviour will be documented after they have been defined in the requirements and implemented.

## Reports and Exit Codes

The planned JSON report will contain:

- The configuration used for the monitoring session.
- A summary of collected samples.
- Timestamped findings.
- An overall monitoring verdict.

The planned process exit codes are:

| Exit Code | Meaning |
|---|---|
| `0` | No stability issue detected. |
| `1` | One or more stability issues detected. |
| `2` | Invalid command-line usage or configuration. |
| `3` | Internal error. |

These codes describe the intended version 1 contract. Their exact behaviour will be verified through automated tests and documented with examples once implemented.

## Testing

The project is planned to include two levels of automated testing:

- **C++ unit tests:** Test core monitoring logic, resource parsing, and related components independently.
- **Python component tests:** Verify the executable's end-to-end behaviour, including process execution and generated reports.

Test framework choices, test commands, and coverage of relevant process scenarios will be documented after the test setup has been implemented.

## Development Environment

Development takes place in an Ubuntu 24.04 Dev Container to provide a reproducible Linux-based environment for contributors using different host operating systems.

The repository contains the Dev Container configuration in the `.devcontainer/` directory.

To use the configured environment:

1. Open the repository in Visual Studio Code.
2. Install the Microsoft Dev Containers extension if it is not already installed.
3. Reopen the project in the development container.

The container provides the current compiler, build tools, Python environment, and testing utilities used for development.

## Project Documentation

Additional documentation will be maintained separately as the project develops:

- **Requirements:** Functional and non-functional requirements.
- **Contributor Documentation:** Development setup, build instructions, and testing workflow.
- **Roadmap:** Planned milestones and potential future improvements.
- **Assignment Essay:** Project approach, technical decisions, trade-offs, and lessons learned.

Links to these documents will be added once the corresponding files are available in the repository.

## Contributing

This project is currently being developed as part of a take-home assignment. A formal contribution workflow has not yet been established.

Development conventions, build instructions, testing procedures, and contribution guidelines will be documented before the project is considered complete.

## Support and Issue Reporting

For this assignment, questions and issues should be directed to the project owner. A public support channel or formal issue-response policy has not been established.

When reporting a problem, include:

- The Linux distribution and version.
- Relevant compiler and toolchain versions.
- The command used.
- The observed behaviour.
- Relevant error messages or output.

Do not include passwords, access tokens, or other sensitive information in issue reports.

## License

No license has been selected for this repository yet.

Until a license is added, no permissions beyond those granted by applicable copyright law should be assumed.

## Documentation Quality Standards

This README follows established documentation best practices and aims to meet the following quality criteria:

- **Clarity:** Information is presented in a clear and understandable way.
- **Accuracy:** Technical descriptions reflect the agreed project scope and current implementation status.
- **Completeness:** The document covers the project's purpose, features, requirements, usage, and development workflow. Implementation-dependent details will be completed as the project evolves.
- **Consistency:** Terminology, formatting, and structure are used consistently throughout the document.
- **Conciseness:** Information is presented in a structured and focused manner without unnecessary repetition.
- **Reproducibility:** Build, installation, and usage instructions will be verified before being presented as working procedures.
- **Maintainability:** The documentation is structured to remain easy to update as the project develops.
- **Transparency:** Planned features, current limitations, and future work are clearly distinguished.

The README will be reviewed and updated throughout development to ensure that its information remains accurate, useful, and consistent with the actual state of the project.