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
    unit/TestDeveloperOffline.cpp
    unit/TestDeveloperPlan.cpp
    unit/TestDiagnosticsExport.cpp
    unit/TestSecretStore.cpp
    unit/TestDiagnosticsManager.cpp
    unit/TestAudioEngine.cpp
    unit/TestAudioPipeline.cpp
    unit/TestGainStage.cpp
    unit/TestAsioChannelForwarding.cpp
    unit/TestAsioDeviceInfo.cpp
    unit/TestNullBackends.cpp
    unit/TestTranslationContract.cpp
    unit/TestLanguageRegistry.cpp
    unit/TestLatencyAccounting.cpp
    unit/TestMockTranslationBackend.cpp
    unit/TestReconnectSupervisor.cpp
    unit/TestApplicationController.cpp
    unit/TestNdiDispatch.cpp
    unit/TestNdiOutput.cpp
    unit/TestNetworkLimits.cpp
    unit/TestSecretStores.cpp
    unit/TestTextPipeline.cpp
    unit/TestTranslationStreamer.cpp
    unit/TestUiModel.cpp
    unit/TestWavFile.cpp
    integration/TestFaultInjection.cpp
    integration/TestTranslationEndToEnd.cpp)

# Design note: the OpenAI backend exists only where its transport (winhttp) exists.
# Its tests also need the nlohmann-free public surface of the Network target and
# the scripted transport fake.
if(WIN32)
    target_sources(lingoflow_tests
        PRIVATE
            support/FakeWebSocketTransport.h
            unit/TestBase64.cpp
            unit/TestPcmResampler.cpp
            unit/TestSafetyIdentifier.cpp
            unit/TestOpenAIProtocol.cpp
            performance/TestCallbackPerf.cpp)
endif()

# The test tree is its own include root: tests include "support/..." by path,
# and the mock deliberately lives here, not in the product core.
target_include_directories(lingoflow_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(lingoflow_tests
    PRIVATE
        lingoflow::core
        Catch2::Catch2WithMain)

if(WIN32)
    target_link_libraries(lingoflow_tests PRIVATE lingoflow_network)
endif()

# Design note: the real NDI output is tested against whatever runtime the build
# machine provides - the same dynamic-load path the product takes. With no NDI
# runtime installed the test asserts the clear "unavailable" answer instead of
# quietly skipping: a missing check is a known state, not a hidden one.
if(TARGET lingoflow_ndi_real)
    target_link_libraries(lingoflow_tests PRIVATE lingoflow_ndi_real)
    target_compile_definitions(lingoflow_tests PRIVATE LINGOFLOW_TESTS_WITH_NDI=1)
    target_include_directories(lingoflow_tests PRIVATE "${LINGOFLOW_NDI_SDK_INCLUDE}")
endif()

if(MSVC)
    target_compile_options(lingoflow_tests PRIVATE /W4 /utf-8 /MP8)
else()
    target_compile_options(lingoflow_tests PRIVATE -Wall -Wextra)
endif()

include(CTest)
include(Catch)
catch_discover_tests(lingoflow_tests)

# --- realtime allocation gate ---------------------------------------
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

# --- realtime safety gates -------------------------------------------
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

# --- ASIO discovery -------------------------------------------------
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
