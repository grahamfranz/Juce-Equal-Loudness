cmake_minimum_required(VERSION 3.22)
project(juce_equal_loudness VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ----------------------------------------------------------------------------
# JUCE
#
# Point CMake at a JUCE source tree (the one you cloned from the JUCE repo).
# Pass it on the command line with -DJUCE_PATH=/path/to/JUCE, or set the
# JUCE_PATH environment variable.
# ----------------------------------------------------------------------------
if(NOT DEFINED JUCE_PATH AND DEFINED ENV{JUCE_PATH})
    set(JUCE_PATH "$ENV{JUCE_PATH}")
endif()

if(NOT JUCE_PATH)
    message(FATAL_ERROR
        "JUCE_PATH is not set. Pass -DJUCE_PATH=/path/to/JUCE on the cmake "
        "command line, or set the JUCE_PATH environment variable to a JUCE "
        "source checkout (the directory containing JUCE's top-level "
        "CMakeLists.txt).")
endif()

if(NOT EXISTS "${JUCE_PATH}/CMakeLists.txt")
    message(FATAL_ERROR
        "JUCE_PATH='${JUCE_PATH}' does not contain a CMakeLists.txt. Point it "
        "at the root of a JUCE source checkout.")
endif()

add_subdirectory("${JUCE_PATH}" "${CMAKE_BINARY_DIR}/JUCE" EXCLUDE_FROM_ALL)

# ----------------------------------------------------------------------------
# The module
# ----------------------------------------------------------------------------
juce_add_module(modules/juce_equal_loudness)

# ----------------------------------------------------------------------------
# Console demo
# ----------------------------------------------------------------------------
juce_add_console_app(equal_loudness_demo
    PRODUCT_NAME "equal_loudness_demo")

target_sources(equal_loudness_demo PRIVATE examples/demo.cpp)

target_compile_definitions(equal_loudness_demo PRIVATE
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0)

target_link_libraries(equal_loudness_demo PRIVATE
    juce_equal_loudness
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# ----------------------------------------------------------------------------
# Tests (GoogleTest, optional)
# ----------------------------------------------------------------------------
option(JUCE_EQUAL_LOUDNESS_BUILD_TESTS "Build juce_equal_loudness unit tests" ON)

if(JUCE_EQUAL_LOUDNESS_BUILD_TESTS)
    find_package(GTest QUIET)

    if(NOT GTest_FOUND)
        include(FetchContent)
        FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG v1.15.2)
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    endif()

    enable_testing()

    add_executable(iso226_tests tests/iso226_tests.cpp)

    target_compile_definitions(iso226_tests PRIVATE
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0)

    target_link_libraries(iso226_tests PRIVATE
        juce_equal_loudness
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

    if(TARGET GTest::gtest_main)
        target_link_libraries(iso226_tests PRIVATE GTest::gtest_main)
    else()
        target_link_libraries(iso226_tests PRIVATE gtest_main)
    endif()

    include(GoogleTest)
    gtest_discover_tests(iso226_tests)
endif()
