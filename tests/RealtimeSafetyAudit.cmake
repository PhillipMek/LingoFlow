# Realtime safety audit (the established PASS criteria: "no allocations/locks/network/UI in
# callback", the project rules).
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
# Every call in an extracted body must also name either a function already in
# rt_audit_rows or an allowed standard/builtin operation (std:: arithmetic, memcpy,
# atomic load/store/...). A new helper on the realtime path that is not added to the
# table in the same commit fails with RT_AUDIT_UNLISTED_CALLEE. This check is lexical
# too: it cannot resolve overloads, follow a function pointer or tell a project name
# from a standard algorithm that happens to share it. The runtime allocation test
# remains the behavioural backstop.
#
# Every failure line starts with a stable RT_AUDIT_* code so the self-test can assert
# on the reason.

# Blank source lines are real elements of the extracted body (CMP0007 NEW), so the
# reported line numbers stay aligned with the file.
cmake_policy(SET CMP0007 NEW)
cmake_policy(SET CMP0057 NEW) # IN_LIST in if(), used by the call-graph check

if(NOT DEFINED RT_AUDIT_SRC_DIR)
    message(FATAL_ERROR "RT_AUDIT_SRC_DIR is not set")
endif()

if(NOT IS_DIRECTORY "${RT_AUDIT_SRC_DIR}")
    message(FATAL_ERROR "RT_AUDIT_SRC_DIR does not exist: ${RT_AUDIT_SRC_DIR}")
endif()

# Rows are "file|marker". The marker is a regex matched against one line of the file:
# the line that opens (or wholly contains) the function body. It never contains a '|'.
#
# A hostile review pass: the callback's reachable set had grown past this table -
# the in-place GainStage wrapper, its targetLinear(), the DiagnosticsManager counters
# processAudio calls every block, and the readable() the jitter policy decides on.
# All of them are rows now, so the gate - not only this file's comment - vouches for
# every function the audio thread can enter.
set(rt_audit_rows
    "Audio/AudioRingBuffer.cpp|AudioRingBuffer::write"
    "Audio/AudioRingBuffer.cpp|AudioRingBuffer::read\\("
    "Audio/AudioRingBuffer.cpp|AudioRingBuffer::readOrSilence"
    "Audio/AudioRingBuffer.cpp|AudioRingBuffer::readable"
    "Audio/AudioRingBuffer.cpp|AudioRingBuffer::discardAll"
    "Audio/AudioJitterBuffer.cpp|AudioJitterBuffer::write"
    "Audio/AudioJitterBuffer.cpp|AudioJitterBuffer::readOrSilence"
    "Audio/AudioJitterBuffer.cpp|AudioJitterBuffer::discard"
    "Audio/AudioJitterBuffer.cpp|AudioJitterBuffer::setTargetFrames"
    "Audio/AudioJitterBuffer.h|std::uint64_t underrunEvents"
    "Audio/AudioEngine.h|OutputRouting outputRouting"
    "Audio/LevelMeter.cpp|LevelMeter::measure"
    "Audio/GainStage.cpp|float GainStage::dbToLinear"
    "Audio/GainStage.cpp|float GainStage::targetLinear"
    "Audio/GainStage.cpp|void GainStage::process\\(float\\* data"
    "Audio/GainStage.cpp|void GainStage::process\\(const float\\* input"
    "Audio/HardGate.cpp|void HardGate::process"
    "Audio/AudioEngine.cpp|AudioEngine::processAudio"
    "Audio/AudioEngine.cpp|void silenceOutputs"
    "Audio/Asio/AsioChannelForwarding.h|ChannelArrayShape forwardActiveChannels"
    "Diagnostics/DiagnosticsManager.h|void countAudioBlock"
    "Diagnostics/DiagnosticsManager.h|void countUnderrun"
    "Diagnostics/DiagnosticsManager.h|void countOverrun"
    "Platform/Asio/JuceAsioBackend.cpp|void audioDeviceIOCallbackWithContext"
    "Platform/Asio/JuceAsioBackend.cpp|void updateArrayEvidence"
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

# Simple names of the audited functions, derived from the table so a renamed row
# cannot silently drop out of the call-graph check. "void GainStage::process(float*"
# yields "process"; "AudioRingBuffer::read\(" yields "read".
set(rt_audit_listed_callees "")
foreach(entry IN LISTS rt_audit_rows)
    string(FIND "${entry}" "|" separator)
    if(separator EQUAL -1)
        continue()
    endif()
    math(EXPR after "${separator} + 1")
    string(SUBSTRING "${entry}" "${after}" -1 marker_text)
    string(REPLACE "\\(" "(" marker_text "${marker_text}")
    string(FIND "${marker_text}" "(" paren)
    if(paren EQUAL -1)
        set(prefix "${marker_text}")
    else()
        string(SUBSTRING "${marker_text}" 0 "${paren}" prefix)
    endif()
    string(REGEX MATCH "[A-Za-z_][A-Za-z0-9_]*$" callee "${prefix}")
    if(NOT callee STREQUAL "")
        list(APPEND rt_audit_listed_callees "${callee}")
    endif()
endforeach()
list(REMOVE_DUPLICATES rt_audit_listed_callees)

# Standard/builtin calls allowed in a callback body. Kept deliberately narrow: an
# unknown name is a failure, not a pass. "get" is the usual smart-pointer/
# optional accessor; "applyOne" is a local lambda whose body is already scanned
# as part of GainStage::process (see the lambda rule below).
set(rt_audit_allowed_callees
    load store exchange fetch_add fetch_sub fetch_or fetch_and
    compare_exchange_strong compare_exchange_weak
    memcpy memmove memset memcmp strlen
    isfinite isnan isinf exp2 exp log log10 pow sqrt fabs
    abs min max clamp floor ceil round
    data size get applyOne)

# Control flow, casts and allocation keywords are not calls for this check; the
# forbidden-token scan above still rejects them where they matter.
set(rt_audit_call_keywords
    if for while switch return sizeof catch throw new delete
    static_cast dynamic_cast reinterpret_cast const_cast
    noexcept decltype alignof typeid operator)

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

        # Call-graph step: every callee must be an audited realtime helper, an
        # allowed builtin, or a control-flow/cast keyword. A helper added to the
        # callback path without a table row fails here even if it contains no
        # forbidden token itself.
        # CMake's regex engine has no POSIX [:space:] class here; matching the
        # immediate name+paren avoids eating a trailing identifier character.
        string(REGEX MATCHALL "[A-Za-z_][A-Za-z0-9_]*\\(" call_tokens "${code}")
        foreach(token IN LISTS call_tokens)
            string(REGEX REPLACE "\\($" "" callee "${token}")
            if(callee STREQUAL "")
                continue()
            endif()
            if(callee IN_LIST rt_audit_call_keywords)
                continue()
            endif()
            if(callee IN_LIST rt_audit_allowed_callees)
                continue()
            endif()
            if(callee IN_LIST rt_audit_listed_callees)
                continue()
            endif()
            list(APPEND rt_audit_failures
                "RT_AUDIT_UNLISTED_CALLEE: ${rel_file}:${line_at} inside '${marker}': '${callee}()' is neither an audited realtime helper nor an allowed builtin")
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
