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

add_executable(liveai_tests
    unit/TestLog.cpp
    unit/TestConfig.cpp
    unit/TestConfigManager.cpp
    unit/TestSecretStore.cpp
    unit/TestDiagnosticsManager.cpp
    unit/TestAudioEngine.cpp
    unit/TestNullBackends.cpp
    unit/TestApplicationController.cpp)

target_link_libraries(liveai_tests
    PRIVATE
        liveai::core
        Catch2::Catch2WithMain)

if(MSVC)
    target_compile_options(liveai_tests PRIVATE /W4 /utf-8 /MP8)
else()
    target_compile_options(liveai_tests PRIVATE -Wall -Wextra)
endif()

include(CTest)
include(Catch)
catch_discover_tests(liveai_tests)

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

set_target_properties(liveai_tests PROPERTIES FOLDER "Tests")
