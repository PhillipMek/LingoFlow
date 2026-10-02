# Realtime safety audit (task 005 PASS criteria: "no allocations/locks/network/UI in
# callback", AGENTS.md 5).
#
# Invoked by CTest as:
#   cmake -DRT_AUDIT_SRC_DIR=<repo>/src -P tests/RealtimeSafetyAudit.cmake
#
# The unit tests prove behaviour; this gate proves the text of the realtime path
# cannot contain a forbidden operation. For every function that runs on the audio
# thread - and every helper it calls - the body is extracted and scanned for
# allocations, locks, sleeping, filesystem, transport, UI and throwing constructs.
#
# Stated limitation: this is a lexical gate. It cannot see a violation hidden behind a
# macro, and it relies on the project's formatting (a function body ends at a "}" or,
# for a class member, at an indented "    }"). That is why every helper reachable from
# the callback is listed explicitly, and why
# tests/realtime/TestRealtimeAllocations.cpp additionally counts heap allocations at
# run time. A function that disappears from the source is an error, not a pass: the
# audit compares the number of bodies it extracted against the table.
#
# Every failure line starts with a stable RT_AUDIT_* code so the self-test can assert
# on the reason.

# Blank source lines are real elements of the extracted body (CMP0007 NEW), so the
# reported line numbers stay aligned with the file.
cmake_policy(SET CMP0007 NEW)

if(NOT DEFINED RT_AUDIT_SRC_DIR)
    message(FATAL_ERROR "RT_AUDIT_SRC_DIR is not set")
endif()

if(NOT IS_DIRECTORY "${RT_AUDIT_SRC_DIR}")
    message(FATAL_ERROR "RT_AUDIT_SRC_DIR does not exist: ${RT_AUDIT_SRC_DIR}")
endif()

# Rows are "file|marker". The marker is a regex matched against one line of the file:
# the line that opens (or wholly contains) the function body. It never contains a '|'.
set(rt_audit_rows
    "Audio/AudioRingBuffer.cpp|AudioRingBuffer::write"
    "Audio/AudioRingBuffer.cpp|AudioRingBuffer::read\\("
    "Audio/AudioRingBuffer.cpp|AudioRingBuffer::readOrSilence"
    "Audio/AudioJitterBuffer.cpp|AudioJitterBuffer::write"
    "Audio/AudioJitterBuffer.cpp|AudioJitterBuffer::readOrSilence"
    "Audio/AudioJitterBuffer.cpp|AudioJitterBuffer::setTargetFrames"
    "Audio/LevelMeter.cpp|LevelMeter::measure"
    "Audio/GainStage.cpp|float GainStage::dbToLinear"
    "Audio/GainStage.cpp|void GainStage::process\\(const float\\* input"
    "Audio/AudioEngine.cpp|AudioEngine::processAudio"
    "Platform/Asio/JuceAsioBackend.cpp|void audioDeviceIOCallbackWithContext"
    "Platform/Asio/JuceAsioBackend.cpp|void audioDeviceError")

# Allocation, blocking, sleeping, I/O, transport, UI, throwing.
set(rt_audit_patterns
    "new |delete |make_unique|make_shared|malloc|calloc|realloc|strdup"
    "std::string|std::to_string|std::wstring|std::format"
    "mutex|lock_guard|scoped_lock|unique_lock|shared_lock|condition_variable|\\.lock\\(\\)|\\.wait\\(|\\.wait_for\\("
    "log::|Logger|spdlog|printf|std::cout|std::cerr"
    "sleep_for|sleep_until|this_thread|Sleep\\("
    "fstream|filesystem|ifstream|ofstream|fopen|CreateFile|WriteFile|RegOpenKey|RegSetValue"
    "nlohmann|json|websocket|WebSocket|curl|WinHTTP|InternetOpen|socket|::send\\(|::recv\\("
    "juce::File|juce::Component|MessageBox|DocumentWindow|repaint|setVisible"
    "\\.reserve\\(|\\.resize\\(|\\.push_back\\(|\\.emplace_back\\(|\\.assign\\(|\\.clear\\(\\)"
    "\\.at\\(|throw |catch *\\(")

list(LENGTH rt_audit_rows rt_audit_expected)

# Count occurrences of a single character. Deliberately not REGEX MATCHALL: under
# CMP0007 NEW an empty match result is still a one-element empty list, which would
# count a brace that is not there.
function(rt_count_char text char out_var)
    string(REPLACE "${char}" "" stripped "${text}")
    string(LENGTH "${text}" with)
    string(LENGTH "${stripped}" without)
    math(EXPR counted "${with} - ${without}")
    set(${out_var} "${counted}" PARENT_SCOPE)
endfunction()

set(rt_audit_functions_audited 0)
set(rt_audit_lines_scanned 0)
set(rt_audit_failures "")

foreach(entry IN LISTS rt_audit_rows)
    string(FIND "${entry}" "|" separator)

    if(separator EQUAL -1)
        list(APPEND rt_audit_failures "RT_AUDIT_BAD_TABLE_ROW: '${entry}' has no '|' separator")
        continue()
    endif()

    string(SUBSTRING "${entry}" 0 "${separator}" rel_file)
    math(EXPR after "${separator} + 1")
    string(SUBSTRING "${entry}" "${after}" -1 marker)

    set(source "${RT_AUDIT_SRC_DIR}/${rel_file}")

    if(NOT EXISTS "${source}")
        list(APPEND rt_audit_failures "RT_AUDIT_MISSING_FILE: ${rel_file} (audited file not found)")
        continue()
    endif()

    file(READ "${source}" content)
    string(REPLACE "\r\n" "\n" content "${content}")

    # A CMake list is also "; "-separated, and C++ lines are full of semicolons: without
    # this placeholder every `;` would become an extra "line" and the reported line
    # numbers would drift away from the file.
    string(REPLACE ";" "@SEMI@" content "${content}")
    string(REPLACE "\n" ";" lines "${content}")

    set(found_index 0)
    set(body "")
    set(line_number 0)
    set(depth 0)
    set(opened FALSE)

    foreach(line IN LISTS lines)
        string(REPLACE "@SEMI@" ";" line "${line}")
        math(EXPR line_number "${line_number} + 1")

        if(found_index EQUAL 0)
            if(line MATCHES "${marker}")
                set(found_index "${line_number}")
                set(body "${line}")

                # Count the braces on the marker line itself. A one-liner (a class
                # member whose whole body sits on that line) opens and closes there;
                # a normal definition line has no brace at all and the scan continues.
                string(REGEX REPLACE "//.*" "" code "${line}")
                rt_count_char("${code}" "{" n_open)
                rt_count_char("${code}" "}" n_close)
                math(EXPR depth "${n_open} - ${n_close}")

                if(n_open GREATER 0)
                    set(opened TRUE)
                endif()

                if(opened AND depth EQUAL 0)
                    break()
                endif()
            endif()

            continue()
        endif()

        # Brace counting, not "first line that looks like a closing brace": the bodies
        # contain nested blocks, and the earlier rule stopped at the first inner "}" and
        # silently skipped the rest of the function. Braces inside string literals would
        # confuse this; none of the audited functions contains one.
        string(REGEX REPLACE "//.*" "" code "${line}")
        rt_count_char("${code}" "{" n_open)
        rt_count_char("${code}" "}" n_close)
        math(EXPR depth "${depth} + ${n_open} - ${n_close}")

        if(n_open GREATER 0)
            set(opened TRUE)
        endif()

        if(depth LESS 0)
            set(depth 0)
        endif()

        list(APPEND body "${line}")

        if(opened AND depth EQUAL 0)
            break()
        endif()
    endforeach()

    if(found_index EQUAL 0)
        list(APPEND rt_audit_failures "RT_AUDIT_FUNCTION_NOT_FOUND: ${rel_file} contains no '${marker}'")
        continue()
    endif()

    list(LENGTH body body_length)

    if(body_length EQUAL 0)
        list(APPEND rt_audit_failures "RT_AUDIT_EMPTY_BODY: ${rel_file} '${marker}' produced no lines")
        continue()
    endif()

    math(EXPR rt_audit_functions_audited "${rt_audit_functions_audited} + 1")
    math(EXPR rt_audit_lines_scanned "${rt_audit_lines_scanned} + ${body_length}")

    # body[0] is the marker line itself, so the i-th entry is at line found_index + i.
    set(cursor -1)

    foreach(line IN LISTS body)
        math(EXPR cursor "${cursor} + 1")
        math(EXPR line_at "${found_index} + ${cursor}")

        # Comments may name forbidden things (they do, in this file); only code counts.
        string(REGEX REPLACE "//.*" "" code "${line}")

        if(code MATCHES "^[ \t]*(\\*|/\\*|//)")
            continue()
        endif()

        foreach(pattern IN LISTS rt_audit_patterns)
            if(code MATCHES "${pattern}")
                string(STRIP "${code}" code)
                list(APPEND rt_audit_failures
                    "RT_AUDIT_FORBIDDEN: ${rel_file}:${line_at} inside '${marker}': '${code}' matches forbidden realtime pattern [${pattern}]")
            endif()
        endforeach()
    endforeach()
endforeach()

if(rt_audit_failures)
    list(JOIN rt_audit_failures "\n  - " failure_text)
    message(FATAL_ERROR
        "realtime safety audit FAILED (${rt_audit_functions_audited} of ${rt_audit_expected} functions, ${rt_audit_lines_scanned} lines):\n  - ${failure_text}")
endif()

if(NOT rt_audit_functions_audited EQUAL rt_audit_expected)
    message(FATAL_ERROR
        "realtime safety audit inspected ${rt_audit_functions_audited} of ${rt_audit_expected} functions")
endif()

message(STATUS
    "realtime safety audit: OK (${rt_audit_functions_audited} realtime functions, ${rt_audit_lines_scanned} lines scanned)")
