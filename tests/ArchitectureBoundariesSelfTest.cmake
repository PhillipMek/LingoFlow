# Self-test for the architecture boundary audit.
#
# A gate that cannot fail is not a gate. This script copies src/ into a scratch
# directory, applies one modification at a time and asserts that
# ArchitectureBoundaries.cmake answers correctly: violations must be rejected with
# the expected AUDIT_* code, and the documented exceptions (nlohmann/json.hpp in
# Config) must still be accepted.
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

# prepare(<kind> <relative path> <text>): rebuild the scratch copy and apply one change
function(prepare kind path text)
    file(REMOVE_RECURSE "${AUDIT_WORK_DIR}")
    file(COPY "${AUDIT_SRC_DIR}" DESTINATION "${AUDIT_WORK_DIR}")

    if(kind STREQUAL "APPEND")
        if(NOT EXISTS "${AUDIT_WORK_DIR}/src/${path}")
            message(FATAL_ERROR "injection target does not exist: ${path}")
        endif()
        file(APPEND "${AUDIT_WORK_DIR}/src/${path}" "\n${text}\n")
    elseif(kind STREQUAL "CREATE")
        file(WRITE "${AUDIT_WORK_DIR}/src/${path}" "${text}\n")
    elseif(NOT kind STREQUAL "CLEAN")
        message(FATAL_ERROR "unknown injection kind '${kind}'")
    endif()
endfunction()

# audit_expect(<label> <PASS|FAIL> <expect-regex> <kind> <path> <text>)
function(audit_expect label verdict expect_regex kind path text)
    prepare("${kind}" "${path}" "${text}")

    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DAUDIT_SRC_DIR=${AUDIT_WORK_DIR}/src" -P "${AUDIT_SCRIPT}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err)

    set(output "${out}${err}")

    if(verdict STREQUAL "PASS")
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "${label}: audit rejected something it must accept:\n${output}")
        endif()
        message(STATUS "${label}: accepted")
        return()
    endif()

    if(result EQUAL 0)
        message(FATAL_ERROR "${label}: audit ACCEPTED a violation it must reject:\n${output}")
    endif()

    if(NOT output MATCHES "${expect_regex}")
        message(FATAL_ERROR "${label}: audit rejected for the wrong reason.\nExpected /${expect_regex}/\nGot:\n${output}")
    endif()

    message(STATUS "${label}: rejected as expected (${expect_regex})")
endfunction()

# --- the untouched tree is the baseline ---------------------------------------
audit_expect("selftest/clean-tree" PASS "" CLEAN "" "")

# --- documented exceptions must keep working ----------------------------------
audit_expect("selftest/json-in-config" PASS "" APPEND "Config/ConfigManager.h"
    "#include <nlohmann/json.hpp>")

# --- direction violations ------------------------------------------------------
audit_expect("selftest/audio-includes-translation" FAIL "AUDIT_INCLUDE_DIRECTION"
    APPEND "Audio/IAudioBackend.h" "#include \"Translation/ITranslationBackend.h\"")

audit_expect("selftest/config-includes-audio" FAIL "AUDIT_INCLUDE_DIRECTION"
    APPEND "Config/ConfigManager.h" "#include \"Audio/AudioEngine.h\"")

audit_expect("selftest/app-includes-nothing-below-is-fine" PASS "" CLEAN "" "")

audit_expect("selftest/diagnostics-includes-config" FAIL "AUDIT_INCLUDE_DIRECTION"
    APPEND "Diagnostics/DiagnosticsManager.h" "#include \"Config/AppConfig.h\"")

audit_expect("selftest/security-includes-config" FAIL "AUDIT_INCLUDE_DIRECTION"
    APPEND "Security/ISecretStore.h" "#include \"Config/ConfigManager.h\"")

# --- JUCE stays in the UI and in the platform adapters ---------------------------
audit_expect("selftest/juce-in-audio" FAIL "AUDIT_JUCE_OUTSIDE_UI"
    APPEND "Audio/AudioTypes.h" "#include <juce_core/juce_core.h>")

audit_expect("selftest/juce-in-config" FAIL "AUDIT_JUCE_OUTSIDE_UI"
    APPEND "Config/AppConfig.h" "#include <juce_data_structures/juce_data_structures.h>")

audit_expect("selftest/juce-in-platform-allowed" PASS ""
    APPEND "Platform/Asio/AsioDiscovery.cpp" "#include <juce_audio_devices/juce_audio_devices.h>")

audit_expect("selftest/asiodiscovery-project-include-allowed" PASS ""
    APPEND "Platform/Asio/AsioDiscovery.cpp" "#include \"Platform/Asio/JuceAsioCommon.h\"")

audit_expect("selftest/platform-includes-app" FAIL "AUDIT_INCLUDE_DIRECTION"
    APPEND "Platform/Asio/JuceAsioBackend.cpp" "#include \"App/ApplicationController.h\"")

audit_expect("selftest/audio-includes-platform" FAIL "AUDIT_INCLUDE_DIRECTION"
    APPEND "Audio/AudioEngine.h" "#include \"Platform/Asio/JuceAsioBackend.h\"")

# The transport-token trap is already covered by the clean-tree case: the project's
# own headers live under Audio/Asio/ and Platform/Asio/ and include each other.

# --- protocol/transport outside the owning module -------------------------------
audit_expect("selftest/json-in-ui" FAIL "AUDIT_PROTOCOL_INCLUDE"
    APPEND "App/Main.cpp" "#include <nlohmann/json.hpp>")

audit_expect("selftest/json-in-audio" FAIL "AUDIT_PROTOCOL_INCLUDE"
    APPEND "Audio/AudioEngine.h" "#include <nlohmann/json.hpp>")

audit_expect("selftest/asio-in-core" FAIL "AUDIT_PROTOCOL_INCLUDE"
    APPEND "Utils/Log.h" "#include <asiosys.h>")

audit_expect("selftest/websocket-in-translation" FAIL "AUDIT_PROTOCOL_INCLUDE"
    APPEND "Translation/ITranslationBackend.h" "#include <websocketpp/client.hpp>")

# --- structural problems -------------------------------------------------------
audit_expect("selftest/dangling-project-include" FAIL "AUDIT_UNRESOLVED_INCLUDE"
    APPEND "Audio/AudioEngine.h" "#include \"Audio/NoSuchFile.h\"")

audit_expect("selftest/unknown-module-directory" FAIL "AUDIT_UNKNOWN_MODULE"
    CREATE "Widgets/Loose.h" "// an unknown top-level directory must be reported")

audit_expect("selftest/file-outside-module" FAIL "AUDIT_LOOSE_FILE"
    CREATE "Loose.cpp" "// a file directly under src/ must be reported")

file(REMOVE_RECURSE "${AUDIT_WORK_DIR}")
message(STATUS "architecture boundary audit self-test: OK")
