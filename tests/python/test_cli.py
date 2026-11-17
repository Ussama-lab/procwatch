import subprocess
from pathlib import Path

import pytest


# Define the project root and the path to the procwatch executable
PROJECT_ROOT = Path(__file__).resolve().parents[2]
PROCWATCH_BINARY = PROJECT_ROOT / "build" / "procwatch"


# Check that the procwatch executable exists before running the tests
@pytest.fixture(scope="session")
def procwatch_binary():
    if not PROCWATCH_BINARY.is_file():
        pytest.fail(
            f"procwatch executable not found: {PROCWATCH_BINARY}. "
            "Build the project before running the tests."
        )

    return PROCWATCH_BINARY


# Check that procwatch starts successfully and prints the startup message
def test_cli_starts_successfully(procwatch_binary):
    result = subprocess.run(
        [str(procwatch_binary)],
        capture_output=True,
        text=True,
        timeout=5,
        check=False,
    )

    assert result.returncode == 0
    assert "Procwatch started!" in result.stdout


# Check that procwatch does not produce error messages during normal startup
def test_cli_does_not_write_errors_on_startup(procwatch_binary):
    result = subprocess.run(
        [str(procwatch_binary)],
        capture_output=True,
        text=True,
        timeout=5,
        check=False,
    )

    assert result.returncode == 0
    assert result.stderr == ""
