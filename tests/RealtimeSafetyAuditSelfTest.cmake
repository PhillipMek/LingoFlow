# Self-test for the realtime safety audit.
#
# A gate that cannot fail is not a gate. This script copies src/ into a scratch
# directory, injects one forbidden construct at a time into the body of a realtime
# function, and asserts that RealtimeSafetyAudit.cmake rejects it with the expected
# RT_AUDIT_* code - and that an untouched copy still passes.
#
#   cmake -DRT_AUDIT_SRC_DIR=<repo>/src
#         -DRT_AUDIT_SCRIPT=<repo>/tests/RealtimeSafetyAudit.cmake
#         -DRT_AUDIT_WORK_DIR=<scratch> -P tests/RealtimeSafetyAuditSelfTest.cmake

foreach(required RT_AUDIT_SRC_DIR RT_AUDIT_SCRIPT RT_AUDIT_WORK_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is not set")
    endif()
endforeach()

if(NOT EXISTS "${RT_AUDIT_SCRIPT}")
    message(FATAL_ERROR "audit script not found: ${RT_AUDIT_SCRIPT}")
endif()

# Inject <code> into the copy after the line of <function> that matches <anchor>.
# rt_expect(<label> <PASS|FAIL> <expect-regex> <relative file> <marker> <anchor> <code>)
function(rt_expect label verdict expect_regex rel_file marker anchor code)
    file(REMOVE_RECURSE "${RT_AUDIT_WORK_DIR}")
    file(COPY "${RT_AUDIT_SRC_DIR}" DESTINATION "${RT_AUDIT_WORK_DIR}")

    set(target "${RT_AUDIT_WORK_DIR}/src/${rel_file}")

    if(NOT code STREQUAL "")
        if(NOT EXISTS "${target}")
            message(FATAL_ERROR "injection target does not exist: ${rel_file}")
        endif()

        file(READ "${target}" content)

        # Literal search, not regex: the anchor is a whole source line with punctuation.
        string(FIND "${content}" "${anchor}" anchor_pos)

        if(anchor_pos EQUAL -1)
            message(FATAL_ERROR "${label}: injection anchor not found in ${rel_file}: ${anchor}")
        endif()

        string(FIND "${content}" "${marker}" marker_pos)

        if(marker_pos EQUAL -1)
            message(FATAL_ERROR "${label}: function marker not found in ${rel_file}: ${marker}")
        endif()

        if(anchor_pos LESS marker_pos)
            message(FATAL_ERROR "${label}: the anchor occurs before the function body, refusing to inject")
        endif()

        string(REPLACE "${anchor}" "${anchor}\n    ${code}" content "${content}")
        file(WRITE "${target}" "${content}")
    endif()

    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DRT_AUDIT_SRC_DIR=${RT_AUDIT_WORK_DIR}/src" -P "${RT_AUDIT_SCRIPT}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err)

    set(output "${out}${err}")

    if(verdict STREQUAL "PASS")
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "${label}: the audit rejected a clean tree:\n${output}")
        endif()

        message(STATUS "${label}: accepted as expected")
        return()
    endif()

    if(result EQUAL 0)
        message(FATAL_ERROR "${label}: the audit ACCEPTED a hard-realtime violation:\n${output}")
    endif()

    if(NOT output MATCHES "${expect_regex}")
        message(FATAL_ERROR "${label}: the audit failed for the wrong reason (expected ${expect_regex}):\n${output}")
    endif()

    message(STATUS "${label}: rejected with ${expect_regex}, as it must")
endfunction()

set(engine "Audio/AudioEngine.cpp")
set(engine_marker "AudioEngine::processAudio")
set(anchor "    blocks_.fetch_add(1, std::memory_order_relaxed);")

set(jitter "Audio/AudioJitterBuffer.cpp")
set(jitter_marker "AudioJitterBuffer::readOrSilence")
set(jitter_anchor "            primingFrames_.fetch_add(frames, std::memory_order_relaxed);")

set(ring "Audio/AudioRingBuffer.cpp")
set(ring_marker "AudioRingBuffer::write")
set(ring_anchor "    std::memcpy(data_.data() + firstIndex, data, firstLength * sizeof(float));")

set(callback "Platform/Asio/JuceAsioBackend.cpp")
set(callback_marker "void audioDeviceIOCallbackWithContext")
set(callback_anchor "        processor_.processAudio(inputViews_.data(), outputViews_.data(), numSamples);")

set(gain "Audio/GainStage.cpp")
set(gain_marker "void GainStage::process(const float* input")
set(gain_anchor "    const float target = targetLinear();")

# 1. The untouched tree must pass: the gate is not broken by construction.
rt_expect("rt-selftest/clean-tree" PASS "" "${engine}" "${engine_marker}" "${anchor}" "")

# 2. Allocation in the callback.
rt_expect("rt-selftest/heap-alloc" FAIL "RT_AUDIT_FORBIDDEN"
    "${engine}" "${engine_marker}" "${anchor}"
    "auto scratch = std::make_unique<float[]>(1024);")

# 3. A lock on the realtime thread.
rt_expect("rt-selftest/mutex" FAIL "RT_AUDIT_FORBIDDEN"
    "${engine}" "${engine_marker}" "${anchor}"
    "std::lock_guard<std::mutex> guard(mutex_);")

# 4. Sleeping instead of returning.
rt_expect("rt-selftest/sleep" FAIL "RT_AUDIT_FORBIDDEN"
    "${engine}" "${engine_marker}" "${anchor}"
    "std::this_thread::sleep_for(std::chrono::milliseconds(1));")

# 5. Logging from the callback.
rt_expect("rt-selftest/logging" FAIL "RT_AUDIT_FORBIDDEN"
    "${jitter}" "${jitter_marker}" "${jitter_anchor}"
    "log::info(\"jitter\", \"block\");")

# 6. Networking in a buffer primitive.
rt_expect("rt-selftest/network" FAIL "RT_AUDIT_FORBIDDEN"
    "${ring}" "${ring_marker}" "${ring_anchor}"
    "socket_ = ::send(sock, data, frames, 0);")

# 7. UI touched on the realtime thread.
rt_expect("rt-selftest/ui" FAIL "RT_AUDIT_FORBIDDEN"
    "${engine}" "${engine_marker}" "${anchor}"
    "window_->repaint();")

# 8. Container growth inside a lock-free primitive.
rt_expect("rt-selftest/container-growth" FAIL "RT_AUDIT_FORBIDDEN"
    "${ring}" "${ring_marker}" "${ring_anchor}"
    "history_.push_back(frames);")

# 9. The JUCE callback bridge is audited too, not only the engine.
rt_expect("rt-selftest/juce-callback" FAIL "RT_AUDIT_FORBIDDEN"
    "${callback}" "${callback_marker}" "${callback_anchor}"
    "std::string name = deviceId_;")

# 9. The gain stage added by task 006 is on the realtime path too: growth there fails.
rt_expect("rt-selftest/gain-stage" FAIL "RT_AUDIT_FORBIDDEN"
    "${gain}" "${gain_marker}" "${gain_anchor}"
    "auto scratch = std::make_unique<float[]>(frames);")

# 10. A dbToLinear that stops being pure math is still a callback function.
rt_expect("rt-selftest/gain-db-to-linear" FAIL "RT_AUDIT_FORBIDDEN"
    "Audio/GainStage.cpp" "float GainStage::dbToLinear" "    return std::exp2(gainDb * kLog2TenOverTwenty);"
    "std::this_thread::sleep_for(std::chrono::milliseconds(1));")

# 11. Task 022: the table now covers the helpers that grew onto the callback path
# later - including inline functions in HEADERS. A lock injected into the counter
# processAudio calls every block must stop the gate.
rt_expect("rt-selftest/diag-counter" FAIL "RT_AUDIT_FORBIDDEN"
    "Diagnostics/DiagnosticsManager.h" "void countAudioBlock"
    "        audioBlocks_.fetch_add(1, std::memory_order_relaxed);"
    "std::lock_guard<std::mutex> guard(mutex_);")

# 12. A function that vanishes must fail the gate, not shrink it silently.
file(REMOVE_RECURSE "${RT_AUDIT_WORK_DIR}")
file(COPY "${RT_AUDIT_SRC_DIR}" DESTINATION "${RT_AUDIT_WORK_DIR}")
file(READ "${RT_AUDIT_WORK_DIR}/src/${engine}" content)
string(REPLACE "AudioEngine::processAudio" "AudioEngine::renamedAway" content "${content}")
file(WRITE "${RT_AUDIT_WORK_DIR}/src/${engine}" "${content}")

execute_process(
    COMMAND "${CMAKE_COMMAND}" "-DRT_AUDIT_SRC_DIR=${RT_AUDIT_WORK_DIR}/src" -P "${RT_AUDIT_SCRIPT}"
    RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)

if(result EQUAL 0)
    message(FATAL_ERROR "rt-selftest/function-vanished: the audit passed although a realtime function disappeared")
endif()

if(NOT "${out}${err}" MATCHES "RT_AUDIT_FUNCTION_NOT_FOUND")
    message(FATAL_ERROR "rt-selftest/function-vanished: expected RT_AUDIT_FUNCTION_NOT_FOUND, got:\n${out}${err}")
endif()

message(STATUS "rt-selftest/function-vanished: rejected with RT_AUDIT_FUNCTION_NOT_FOUND, as it must")

file(REMOVE_RECURSE "${RT_AUDIT_WORK_DIR}")
message(STATUS "realtime safety audit self-test: OK")
