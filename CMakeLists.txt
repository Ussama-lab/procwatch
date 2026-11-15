cmake_minimum_required(VERSION 3.16)

project(procwatch
    VERSION 1.0.0
    LANGUAGES CXX
)

# Use C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enable testing
include(CTest)

# Include C++ dependency management
include(FetchContent)

# GoogleTest dependency
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

# JSON library dependency
FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

# Download and make dependencies available
FetchContent_MakeAvailable(
    googletest
    nlohmann_json
)