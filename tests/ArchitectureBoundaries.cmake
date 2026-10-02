# Architecture boundary audit (task 002 PASS criteria: "dependency direction is
# clean", "UI does not own protocol code", "audio boundary is clean").
#
# Invoked by CTest as:
#   cmake -DAUDIT_SRC_DIR=<repo>/src -P tests/ArchitectureBoundaries.cmake
#
# Only #include directives are inspected (comments may mention anything).
#   1. A module may include only the modules listed in AUDIT_ALLOWED_<module>.
#   2. JUCE headers are allowed only under src/App - the UI layer.
#   3. Protocol/transport headers (openai, websocket, json, asio, curl) are not
#      allowed anywhere under src at this stage. Task 004 introduces the ASIO
#      boundary and task 009 the OpenAI backend; when that happens a dedicated
#      module is added to the tables below instead of relaxing the rule.
#
# Every failure line starts with a stable AUDIT_* code so the self-test can assert
# on the reason without depending on prose or CMake's message wrapping.

if(NOT DEFINED AUDIT_SRC_DIR)
    message(FATAL_ERROR "AUDIT_SRC_DIR is not set")
endif()

if(NOT IS_DIRECTORY "${AUDIT_SRC_DIR}")
    message(FATAL_ERROR "AUDIT_SRC_DIR does not exist: ${AUDIT_SRC_DIR}")
endif()

set(audit_known_modules Utils Config Security Diagnostics Audio Translation NDI App)

set(AUDIT_ALLOWED_Utils       "Utils")
set(AUDIT_ALLOWED_Config      "Config;Utils")
set(AUDIT_ALLOWED_Security    "Security;Utils")
set(AUDIT_ALLOWED_Diagnostics "Diagnostics;Utils")
set(AUDIT_ALLOWED_Audio       "Audio;Utils;Diagnostics")
set(AUDIT_ALLOWED_Translation "Translation;Utils;Diagnostics")
set(AUDIT_ALLOWED_NDI         "NDI;Utils;Diagnostics")
set(AUDIT_ALLOWED_App         "App;Audio;Translation;NDI;Config;Security;Diagnostics;Utils")

# Protocol/transport headers are forbidden under src/ except in the module that
# owns them. Config owns JSON text for config.json (nlohmann/json.hpp, used in a
# .cpp only). Task 009 will add the OpenAI backend module with its own exception;
# until then nothing else may reference a wire protocol.
set(AUDIT_ALLOWED_PROTOCOL_Config "nlohmann/json.hpp")

set(audit_juce_pattern "^juce|juceheader")
set(audit_protocol_pattern "openai|websocket|nlohmann|asio|curl|json")
set(audit_include_pattern "^[ \t]*#[ \t]*include[ \t]*[<\"]([^\">]*)[\">]")

file(GLOB_RECURSE audit_sources RELATIVE "${AUDIT_SRC_DIR}"
    "${AUDIT_SRC_DIR}/*.h"
    "${AUDIT_SRC_DIR}/*.hpp"
    "${AUDIT_SRC_DIR}/*.cpp"
    "${AUDIT_SRC_DIR}/*.cc")

list(LENGTH audit_sources audit_count)

if(audit_count EQUAL 0)
    message(FATAL_ERROR "no sources found under ${AUDIT_SRC_DIR}")
endif()

set(audit_failures "")
set(audit_include_total 0)

foreach(source IN LISTS audit_sources)
    string(FIND "${source}" "/" slash_pos)

    if(slash_pos EQUAL -1)
        list(APPEND audit_failures "AUDIT_LOOSE_FILE: ${source} must live inside a module directory")
        continue()
    endif()

    string(SUBSTRING "${source}" 0 "${slash_pos}" module)

    list(FIND audit_known_modules "${module}" known_index)

    if(known_index EQUAL -1)
        list(APPEND audit_failures "AUDIT_UNKNOWN_MODULE: ${source} is under unknown module '${module}/'")
        continue()
    endif()

    file(READ "${AUDIT_SRC_DIR}/${source}" content)
    string(REPLACE "\r\n" "\n" content "${content}")
    string(REPLACE "\n" ";" content_lines "${content}")

    foreach(line IN LISTS content_lines)
        if(NOT line MATCHES "${audit_include_pattern}")
            continue()
        endif()

        set(target "${CMAKE_MATCH_1}")
        math(EXPR audit_include_total "${audit_include_total} + 1")

        if(target STREQUAL "")
            continue()
        endif()

        string(TOLOWER "${target}" target_lower)

        # 1. protocol/transport leakage
        if(target_lower MATCHES "${audit_protocol_pattern}")
            set(allowed_protocol "${AUDIT_ALLOWED_PROTOCOL_${module}}")
            list(FIND allowed_protocol "${target_lower}" protocol_index)

            if(protocol_index EQUAL -1)
                list(APPEND audit_failures
                    "AUDIT_PROTOCOL_INCLUDE: ${source} includes <${target}>; protocol/transport headers are not allowed in module '${module}/'")
            endif()
            continue()
        endif()

        # 2. JUCE belongs to the UI layer only
        if(target_lower MATCHES "${audit_juce_pattern}")
            if(NOT module STREQUAL "App")
                list(APPEND audit_failures "AUDIT_JUCE_OUTSIDE_APP: ${source} includes <${target}>")
            endif()
            continue()
        endif()

        # 3. project headers must respect the module direction
        if(target MATCHES "^([A-Za-z][A-Za-z0-9_]*)/(.+)$")
            set(included_module "${CMAKE_MATCH_1}")

            list(FIND audit_known_modules "${included_module}" included_index)

            if(included_index EQUAL -1)
                list(APPEND audit_failures "AUDIT_UNKNOWN_MODULE_ROOT: ${source} includes <${target}>")
                continue()
            endif()

            if(NOT EXISTS "${AUDIT_SRC_DIR}/${target}")
                list(APPEND audit_failures "AUDIT_UNRESOLVED_INCLUDE: ${source} includes '${target}' which does not resolve")
                continue()
            endif()

            set(allowed_targets "${AUDIT_ALLOWED_${module}}")
            list(FIND allowed_targets "${included_module}" allowed_index)

            if(allowed_index EQUAL -1)
                list(APPEND audit_failures
                    "AUDIT_INCLUDE_DIRECTION: ${source} includes '${included_module}/' from '${module}/' (allowed: ${allowed_targets})")
            endif()
            continue()
        endif()

        # 4. a path that is neither a module nor a system header
        if(target MATCHES "/")
            list(APPEND audit_failures "AUDIT_UNKNOWN_INCLUDE_PATH: ${source} includes '${target}'")
        endif()

        # anything else is a system/standard header: allowed
    endforeach()
endforeach()

if(audit_include_total EQUAL 0)
    message(FATAL_ERROR
        "AUDIT_BROKEN_PATTERN: parsed ${audit_count} files but found no #include directives")
endif()

if(audit_failures)
    list(JOIN audit_failures "\n  - " failure_text)
    message(FATAL_ERROR "architecture boundary audit FAILED:\n  - ${failure_text}")
endif()

list(JOIN audit_known_modules ", " modules_text)
message(STATUS
    "architecture boundary audit: OK (${audit_count} files, ${audit_include_total} includes, modules: ${modules_text})")
