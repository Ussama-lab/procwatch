
FROM mcr.microsoft.com/devcontainers/base:ubuntu-24.04

USER root

# Install the tools needed to develop, build and test procwatch
RUN apt-get update && apt-get install -y --no-install-recommends \
    # C++ compiler and standard build tools (GCC, G++, Make)
    build-essential \
    # Build system used to configure and compile the project
    cmake \
    # Fast build tool used as a CMake generator
    ninja-build \
    # Debugger for investigating C++ runtime errors
    gdb \
    # Version control for tracking project changes
    git \
    # Python interpreter for component tests
    python3 \
    # Python package installer for additional test dependencies
    python3-pip \
    # Allows isolated Python environments for testing
    python3-venv \
    # Test framework for Python component tests
    python3-pytest \
    # Remove package lists to keep the image smaller
    && rm -rf /var/lib/apt/lists/*

# Use the regular development user instead of root
USER vscode
