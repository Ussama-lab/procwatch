cmake_minimum_required(VERSION 3.24)

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

# Main application
add_executable(procwatch
    src/main.cpp
    src/supervisor.cpp
    src/proc_reader.cpp
    src/sampler.cpp
    src/analyzer.cpp
    src/report.cpp
)

# Include directories
target_include_directories(procwatch
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

# Link dependencies
target_link_libraries(procwatch
    PRIVATE
        nlohmann_json::nlohmann_json
)

# Compiler warnings
target_compile_options(procwatch
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

# Find Python interpreter
find_package(Python3 COMPONENTS Interpreter REQUIRED)

# Python component tests
if(BUILD_TESTING)
    add_test(
        NAME python_component_tests
        COMMAND ${Python3_EXECUTABLE} -m pytest
                ${PROJECT_SOURCE_DIR}/tests/python
    )
endif()

# C++ unit tests
if(BUILD_TESTING)
    add_executable(procwatch_tests
        tests/cpp/test_analyzer.cpp
        tests/cpp/test_proc_reader.cpp
        tests/cpp/test_report.cpp
        tests/cpp/test_sampler.cpp
        tests/cpp/test_supervisor.cpp

        src/proc_reader.cpp
        src/sampler.cpp
        src/analyzer.cpp
        src/report.cpp
    )

    target_link_libraries(procwatch_tests
        PRIVATE
            GTest::gtest_main
            nlohmann_json::nlohmann_json
    )

    target_include_directories(procwatch_tests
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    # Compiler warnings for unit tests
target_compile_options(procwatch_tests
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

    include(GoogleTest)
    gtest_discover_tests(procwatch_tests)
endif()