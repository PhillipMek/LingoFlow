cmake_minimum_required(VERSION 3.25)

project(LiveAIInterpreter
    VERSION 0.1.0
    DESCRIPTION "Live AI Interpreter - simultaneous speech translation for live events"
    LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Language / build defaults
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(LIVEAI_BUILD_TESTS "Build automated tests" ON)

include(FetchContent)

# ---------------------------------------------------------------------------
# JUCE
#
# JUCE is used from a local source tree: it is not vendored into this repo and
# never downloaded automatically. Point LIVEAI_JUCE_PATH at the JUCE checkout.
# If it is not set, we look for the sibling directory documented in
# docs/environment-report.md.
# ---------------------------------------------------------------------------
set(LIVEAI_JUCE_PATH "" CACHE PATH "Path to a local JUCE source tree (folder containing CMakeLists.txt)")

if(LIVEAI_JUCE_PATH STREQUAL "")
    set(_liveai_juce_guess "${CMAKE_CURRENT_LIST_DIR}/../juce-9.0.3-windows/JUCE")
    if(EXISTS "${_liveai_juce_guess}/CMakeLists.txt")
        set(LIVEAI_JUCE_PATH "${_liveai_juce_guess}")
    endif()
endif()

if(NOT EXISTS "${LIVEAI_JUCE_PATH}/CMakeLists.txt")
    message(FATAL_ERROR
        "JUCE not found. Configure with -DLIVEAI_JUCE_PATH=<path to JUCE>, e.g. "
        "-DLIVEAI_JUCE_PATH=C:/src/JUCE (folder must contain CMakeLists.txt).")
endif()

file(READ "${LIVEAI_JUCE_PATH}/CMakeLists.txt" _liveai_juce_cmakelists LIMIT 4096)
string(REGEX MATCH "project\\(JUCE[^)]*VERSION ([0-9]+\\.[0-9]+\\.[0-9]+)" _liveai_juce_ver_match "${_liveai_juce_cmakelists}")
set(LIVEAI_JUCE_VERSION "unknown")
if(CMAKE_MATCH_1)
    set(LIVEAI_JUCE_VERSION "${CMAKE_MATCH_1}")
endif()

message(STATUS "LiveAIInterpreter: using JUCE ${LIVEAI_JUCE_VERSION} from ${LIVEAI_JUCE_PATH}")

# JUCE needs C as a language; it is enabled in project() above.
add_subdirectory("${LIVEAI_JUCE_PATH}" JUCE)

# ---------------------------------------------------------------------------
# nlohmann/json - used ONLY by the Config module (see tests/ArchitectureBoundaries.cmake).
# Pinned to an official release tag; fetched on demand, never vendored.
# ---------------------------------------------------------------------------
option(LIVEAI_FETCH_NLOHMANN_JSON "Download nlohmann/json with FetchContent" ON)

if(LIVEAI_FETCH_NLOHMANN_JSON)
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG        v3.12.0
        GIT_SHALLOW    TRUE)

    set(JSON_BuildTests OFF CACHE INTERNAL "")
    set(INSTALL_NLOHMANN_JSON OFF CACHE INTERNAL "")
    FetchContent_MakeAvailable(nlohmann_json)
    message(STATUS "LiveAIInterpreter: using nlohmann/json ${nlohmann_json_VERSION} from FetchContent")
else()
    find_package(nlohmann_json REQUIRED)
endif()

# ---------------------------------------------------------------------------
# Project
# ---------------------------------------------------------------------------
add_subdirectory(src)

if(LIVEAI_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

message(STATUS "LiveAIInterpreter ${PROJECT_VERSION}: configure done")
