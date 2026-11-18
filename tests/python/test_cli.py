import json
import subprocess
from pathlib import Path

import pytest


# Define the project root and the path to the procwatch executable
PROJECT_ROOT = Path(__file__).resolve().parents[2]
PROCWATCH_BINARY = PROJECT_ROOT / "build" / "procwatch"

EXIT_SUCCESS = 0
EXIT_USAGE_ERROR = 2


# Check that the procwatch executable exists before running the tests
@pytest.fixture(scope="session")
def procwatch_binary():
    if not PROCWATCH_BINARY.is_file():
        pytest.fail(
            f"procwatch executable not found: {PROCWATCH_BINARY}. "
            "Build the project before running the tests."
        )

    return PROCWATCH_BINARY


# Run procwatch in a temporary directory, so a default report stays out of
# the project
def run_procwatch(binary, working_directory, *arguments):
    return subprocess.run(
        [str(binary), *arguments],
        cwd=working_directory,
        capture_output=True,
        text=True,
        timeout=10,
        check=False,
    )


# Check that both help options describe the command line and the exit codes
@pytest.mark.parametrize("help_option", ["--help", "-h"])
def test_help_prints_usage(procwatch_binary, tmp_path, help_option):
    result = run_procwatch(procwatch_binary, tmp_path, help_option)

    assert result.returncode == EXIT_SUCCESS
    assert "Usage: procwatch [options] -- <command> [args...]" in result.stdout
    assert "--max-duration-ms" in result.stdout
    assert "Exit codes:" in result.stdout
    assert result.stderr == ""
    assert list(tmp_path.iterdir()) == []


# Check that a successful target produces a stable result and a valid report
def test_valid_command_writes_stable_report(procwatch_binary, tmp_path):
    report_path = tmp_path / "report.json"

    result = run_procwatch(
        procwatch_binary,
        tmp_path,
        "--report-path",
        str(report_path),
        "--",
        "/bin/true",
    )

    assert result.returncode == EXIT_SUCCESS
    assert "Process exited with code 0" in result.stdout
    assert "Verdict: stable" in result.stdout
    assert f"Report: {report_path}" in result.stdout
    assert result.stderr == ""

    report = json.loads(report_path.read_text(encoding="utf-8"))

    assert report["config"]["command"] == ["/bin/true"]
    assert report["process"] == {
        "termination": "exited",
        "exit_code": 0,
        "signal": None,
    }
    assert report["findings"] == []
    assert report["stable"] is True


# Check that an incomplete command line is rejected before anything is
# launched and that no report is written
@pytest.mark.parametrize(
    ("arguments", "expected_error"),
    [
        pytest.param([], "missing '--'", id="no-arguments"),
        pytest.param(
            ["/bin/true"],
            "use '--' before the command",
            id="missing-separator",
        ),
        pytest.param(
            ["--report-path", "report.json"],
            "missing '--'",
            id="options-without-command",
        ),
        pytest.param(["--"], "missing command after '--'", id="missing-command"),
        pytest.param(
            ["--report-path", "report.json", "--"],
            "missing command after '--'",
            id="options-with-missing-command",
        ),
    ],
)
def test_incomplete_command_line_is_rejected(
    procwatch_binary,
    tmp_path,
    arguments,
    expected_error,
):
    result = run_procwatch(procwatch_binary, tmp_path, *arguments)

    assert result.returncode == EXIT_USAGE_ERROR
    assert result.stdout == ""
    assert expected_error in result.stderr
    assert "Try 'procwatch --help' for more information." in result.stderr
    assert list(tmp_path.iterdir()) == []