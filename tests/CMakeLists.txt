# Tests use Catch2 v3, fetched on demand (no network is needed once the build
# directory is populated). Set -DLIVEAI_FETCH_CATCH2=OFF to use an installed
# Catch2 via find_package instead.
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

set_target_properties(liveai_tests PROPERTIES FOLDER "Tests")
