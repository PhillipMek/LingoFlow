# Tests use Catch2 v3, fetched on demand (no network is needed once the build
# directory is populated). Set -DLIVEAI_FETCH_CATCH2=OFF to use an installed
# Catch2 instead.
include(FetchContent)

option(LIVEAI_FETCH_CATCH2 "Download Catch2 with FetchContent" ON)

if(LIVEAI_FETCH_CATCH2)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.16.0
        GIT_SHALLOW    TRUE)

    set(CATCH_BUILD_TESTING OFF CACHE INTERNAL "")
    set(CATCH_BUILD_EXAMPLES OFF CACHE INTERNAL "")
    set(CATCH_BUILD_EXTRA_EXAMPLES OFF CACHE INTERNAL "")
    set(CATCH_INSTALL_DOCS OFF CACHE INTERNAL "")
    set(CATCH_INSTALL_EXTRAS ON CACHE INTERNAL "")
    set(CATCH_DEVELOPMENT_BUILD OFF CACHE INTERNAL "")

    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
else()
    find_package(Catch2 3 REQUIRED)
endif()

add_executable(lingoflow_tests
    support/MockTranslationBackend.cpp
    unit/TestLog.cpp
    unit/TestConfig.cpp
    unit/TestConfigManager.cpp
    unit/TestSecretStore.cpp
    unit/TestDiagnosticsManager.cpp
    unit/TestAudioEngine.cpp
    unit/TestAudioPipeline.cpp
    unit/TestGainStage.cpp
    unit/TestAsioDeviceInfo.cpp
    unit/TestNullBackends.cpp
    unit/TestTranslationContract.cpp
    unit/TestApplicationController.cpp
    integration/TestTranslationEndToEnd.cpp)

# The test tree is its own include root: tests include "support/..." by path,
# and the mock deliberately lives here, not in the product core (task 007).
target_include_directories(lingoflow_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(lingoflow_tests
    PRIVATE
        lingoflow::core
        Catch2::Catch2WithMain)

if(MSVC)
    target_compile_options(lingoflow_tests PRIVATE /W4 /utf-8 /MP8)
else()
    target_compile_options(lingoflow_tests PRIVATE -Wall -Wextra)
endif()

include(CTest)
include(Catch)
catch_discover_tests(lingoflow_tests)

# --- realtime allocation gate (task 005) ---------------------------------------
# A separate executable, because it replaces global operator new to count heap
# allocations. Isolated so no other suite's behaviour changes.
add_executable(lingoflow_realtime_tests
    realtime/TestRealtimeAllocations.cpp)

target_link_libraries(lingoflow_realtime_tests
    PRIVATE
        lingoflow::core
        Catch2::Catch2WithMain)

if(MSVC)
    target_compile_options(lingoflow_realtime_tests PRIVATE /W4 /utf-8 /MP8)
else()
    target_compile_options(lingoflow_realtime_tests PRIVATE -Wall -Wextra)
endif()

catch_discover_tests(lingoflow_realtime_tests)

# --- architecture boundary gates ---------------------------------------------
# 1) audit the real source tree; 2) prove the audit can fail (self-test).
# These run without Catch2 and without a device, so they are usable in CI.
add_test(NAME architecture_boundary_audit
         COMMAND ${CMAKE_COMMAND}
                 -DAUDIT_SRC_DIR=${CMAKE_SOURCE_DIR}/src
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/ArchitectureBoundaries.cmake)

add_test(NAME architecture_boundary_audit_selftest
         COMMAND ${CMAKE_COMMAND}
                 -DAUDIT_SRC_DIR=${CMAKE_SOURCE_DIR}/src
                 -DAUDIT_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/ArchitectureBoundaries.cmake
                 -DAUDIT_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/audit-selftest
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/ArchitectureBoundariesSelfTest.cmake)

set_property(TEST architecture_boundary_audit architecture_boundary_audit_selftest
             PROPERTY LABELS architecture)

# --- realtime safety gates (task 005) -------------------------------------------
# 1) scan the realtime functions of the real source tree for forbidden operations;
# 2) prove that gate can still fail by injecting each forbidden class into a copy.
add_test(NAME realtime_safety_audit
         COMMAND ${CMAKE_COMMAND}
                 -DRT_AUDIT_SRC_DIR=${CMAKE_SOURCE_DIR}/src
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/RealtimeSafetyAudit.cmake)

add_test(NAME realtime_safety_audit_selftest
         COMMAND ${CMAKE_COMMAND}
                 -DRT_AUDIT_SRC_DIR=${CMAKE_SOURCE_DIR}/src
                 -DRT_AUDIT_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/RealtimeSafetyAudit.cmake
                 -DRT_AUDIT_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/rt-audit-selftest
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/RealtimeSafetyAuditSelfTest.cmake)

set_property(TEST realtime_safety_audit realtime_safety_audit_selftest
             PROPERTY LABELS realtime)

# --- ASIO discovery (task 004) -------------------------------------------------
# The probe tool exercises the real JUCE ASIO path. --verify compares the devices JUCE
# enumerates with the HKLM\SOFTWARE\ASIO registrations, without loading any driver, so
# it is safe in an automated run. Opening a device is NOT part of it: on a machine with
# the SoundGrid driver but no server that needs a human (docs/device-defaults.md).
add_test(NAME asio_discovery_verify
         COMMAND $<TARGET_FILE:lingoflow_asio_probe> --verify)

set_property(TEST asio_discovery_verify PROPERTY LABELS device)
set_property(TEST asio_discovery_verify PROPERTY TIMEOUT 120)

add_test(NAME asio_discovery_list
         COMMAND $<TARGET_FILE:lingoflow_asio_probe> --list)

set_property(TEST asio_discovery_list PROPERTY LABELS device)
set_property(TEST asio_discovery_list PROPERTY TIMEOUT 120)

set_target_properties(lingoflow_tests lingoflow_realtime_tests PROPERTIES FOLDER "Tests")
