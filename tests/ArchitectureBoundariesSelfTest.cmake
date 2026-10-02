# Self-test for the architecture boundary audit.
#
# A gate that cannot fail is not a gate. This script copies src/ into a scratch
# directory, injects one violation at a time and asserts that
# ArchitectureBoundaries.cmake rejects it with the expected reason - and that the
# pristine copy is accepted.
#
#   cmake -DAUDIT_SRC_DIR=<repo>/src -DAUDIT_SCRIPT=<repo>/tests/ArchitectureBoundaries.cmake
#         -DAUDIT_WORK_DIR=<scratch> -P tests/ArchitectureBoundariesSelfTest.cmake

foreach(required AUDIT_SRC_DIR AUDIT_SCRIPT AUDIT_WORK_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is not set")
    endif()
endforeach()

if(NOT EXISTS "${AUDIT_SCRIPT}")
    message(FATAL_ERROR "audit script not found: ${AUDIT_SCRIPT}")
endif()

set(violations_found 0)

# audit_case(<label> <CLEAN|APPEND|CREATE> <relative path> <text> <expected message regex>)
function(audit_case label kind path text expect_regex)
    file(REMOVE_RECURSE "${AUDIT_WORK_DIR}")
    file(COPY "${AUDIT_SRC_DIR}" DESTINATION "${AUDIT_WORK_DIR}")
    set(copied_src "${AUDIT_WORK_DIR}/src")

    if(kind STREQUAL "APPEND")
        if(NOT EXISTS "${copied_src}/${path}")
            message(FATAL_ERROR "${label}: injection target does not exist: ${path}")
        endif()
        file(READ "${copied_src}/${path}" original)
        file(APPEND "${copied_src}/${path}" "\n${text}\n")
    elseif(kind STREQUAL "CREATE")
        get_filename_component(parent "${copied_src}/${path}" DIRECTORY)
        file(WRITE "${copied_src}/${path}" "${text}\n")
    elseif(NOT kind STREQUAL "CLEAN")
        message(FATAL_ERROR "${label}: unknown injection kind '${kind}'")
    endif()

    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DAUDIT_SRC_DIR=${copied_src}" -P "${AUDIT_SCRIPT}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err)

    set(output "${out}${err}")

    if(kind STREQUAL "CLEAN")
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "${label}: audit rejected a clean tree:\n${output}")
        endif()
        message(STATUS "${label}: clean tree accepted")
        return()
    endif()

    if(result EQUAL 0)
        message(FATAL_ERROR "${label}: audit ACCEPTED a violation it must reject:\n${output}")
    endif()

    if(NOT output MATCHES "${expect_regex}")
        message(FATAL_ERROR "${label}: audit rejected for the wrong reason.\nExpected /${expect_regex}/\nGot:\n${output}")
    endif()

    message(STATUS "${label}: violation correctly rejected")
endfunction()

audit_case("selftest/clean" CLEAN "" "" "")

audit_case("selftest/audio-includes-translation" APPEND "Audio/IAudioBackend.h"
    "#include \"Translation/ITranslationBackend.h\""
    "AUDIT_INCLUDE_DIRECTION")

audit_case("selftest/config-includes-audio" APPEND "Config/ConfigManager.h"
    "#include \"Audio/AudioEngine.h\""
    "AUDIT_INCLUDE_DIRECTION")

audit_case("selftest/juce-in-audio" APPEND "Audio/AudioTypes.h"
    "#include <juce_core/juce_core.h>"
    "AUDIT_JUCE_OUTSIDE_APP")

audit_case("selftest/protocol-include-in-ui" APPEND "App/Main.cpp"
    "#include <nlohmann/json.hpp>"
    "AUDIT_PROTOCOL_INCLUDE")

audit_case("selftest/asio-include-in-core" APPEND "Utils/Log.h"
    "#include <asiosys.h>"
    "AUDIT_PROTOCOL_INCLUDE")

audit_case("selftest/dangling-project-include" APPEND "Audio/AudioEngine.h"
    "#include \"Audio/NoSuchFile.h\""
    "AUDIT_UNRESOLVED_INCLUDE")

audit_case("selftest/unknown-module-directory" CREATE "Widgets/Loose.h"
    "// an unknown top-level directory must be reported"
    "AUDIT_UNKNOWN_MODULE")

audit_case("selftest/file-outside-module" CREATE "Loose.cpp"
    "// a file directly under src/ must be reported"
    "AUDIT_LOOSE_FILE")

file(REMOVE_RECURSE "${AUDIT_WORK_DIR}")
message(STATUS "architecture boundary audit self-test: OK (9 cases)")
