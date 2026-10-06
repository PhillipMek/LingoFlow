cmake_minimum_required(VERSION 3.25)

project(LingoFlow
    VERSION 0.1.0
    DESCRIPTION "LingoFlow - simultaneous speech translation for live events"
    LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Language / build defaults
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Task 026 (release): the shipped executable must start on a clean Windows
# machine with no developer dependencies (the task's FAIL criterion). The
# dependency scan of the old dynamic-CRT build measured imports of MSVCP140.dll,
# VCRUNTIME140.dll and VCRUNTIME140_1.dll - the Visual C++ redistributable, i.e.
# exactly such a developer dependency. Static linkage removes it: the Release
# exe then imports system DLLs only (verified by dumpbin after the change, and
# pinned as a check in packaging/release-package.ps1).
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(LIVEAI_BUILD_TESTS "Build automated tests" ON)

include(FetchContent)

# ---------------------------------------------------------------------------
# JUCE
#
# JUCE 9.0.3 is vendored in third_party/JUCE (see third_party/README.md and
# docs/licensing.md - AGPLv3 chosen). LIVEAI_JUCE_PATH can point at another local
# checkout, e.g. for experiments with a different JUCE version; nothing is ever
# downloaded automatically.
# ---------------------------------------------------------------------------
set(LIVEAI_JUCE_PATH "" CACHE PATH "Path to a local JUCE source tree (folder containing CMakeLists.txt)")

if(LIVEAI_JUCE_PATH STREQUAL "")
    set(LIVEAI_JUCE_PATH "${CMAKE_CURRENT_LIST_DIR}/third_party/JUCE")
endif()

if(NOT EXISTS "${LIVEAI_JUCE_PATH}/CMakeLists.txt")
    message(FATAL_ERROR
        "JUCE not found at ${LIVEAI_JUCE_PATH}. Expected the vendored tree in third_party/JUCE, "
        "or pass -DLIVEAI_JUCE_PATH=<path to JUCE> (folder must contain CMakeLists.txt).")
endif()

file(READ "${LIVEAI_JUCE_PATH}/CMakeLists.txt" _liveai_juce_cmakelists LIMIT 4096)
string(REGEX MATCH "project\\(JUCE[^)]*VERSION ([0-9]+\\.[0-9]+\\.[0-9]+)" _liveai_juce_ver_match "${_liveai_juce_cmakelists}")
set(LIVEAI_JUCE_VERSION "unknown")
if(CMAKE_MATCH_1)
    set(LIVEAI_JUCE_VERSION "${CMAKE_MATCH_1}")
endif()

message(STATUS "LingoFlow: using JUCE ${LIVEAI_JUCE_VERSION} from ${LIVEAI_JUCE_PATH}")

# JUCE needs C as a language; it is enabled in project() above.
add_subdirectory("${LIVEAI_JUCE_PATH}" JUCE)

# ---------------------------------------------------------------------------
# nlohmann/json - used by the Config module and by the Network backend
# (docs/openai-realtime-protocol.md consumer, task 009); both link it PRIVATE,
# so no other module can start parsing JSON (tests/ArchitectureBoundaries.cmake).
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
    message(STATUS "LingoFlow: using nlohmann/json ${nlohmann_json_VERSION} from FetchContent")
else()
    find_package(nlohmann_json REQUIRED)
endif()

# ---------------------------------------------------------------------------
# Project
# ---------------------------------------------------------------------------

# NDI SDK headers (task 016) are resolved once here so both src/ (targets) and
# tests/ (the live-output test) see the same path. License rule reminder: these
# headers are for COMPILING only; the import library is never linked
# (docs/licensing.md) - the runtime DLL is loaded at run time.
set(LINGOFLOW_NDI_SDK_INCLUDE "" CACHE PATH "NDI SDK Include directory (default: %NDI_SDK_DIR%/Include)")

if(LINGOFLOW_NDI_SDK_INCLUDE STREQUAL "" AND DEFINED ENV{NDI_SDK_DIR})
    set(LINGOFLOW_NDI_SDK_INCLUDE "$ENV{NDI_SDK_DIR}/Include")
endif()

add_subdirectory(src)

if(LIVEAI_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

message(STATUS "LingoFlow ${PROJECT_VERSION}: configure done")
