#include "Diagnostics/DiagnosticsManager.h"

#include <mutex>
#include <utility>

#include "Utils/Log.h"

namespace liveai {

void DiagnosticsManager::appendEventLocked(std::string_view subsystem, std::string_view message)
{
    events_.push_back(DiagnosticEvent { ++eventSequence_, log::timestampNow(),
                                         std::string(subsystem), std::string(message) });

    while (events_.size() > kMaxEvents)
    {
        events_.pop_front();
        evictedEvents_.fetch_add(1, std::memory_order_relaxed);
    }
}

void DiagnosticsManager::noteAudioBackend(std::string_view backendName)
{
    std::lock_guard lock(textMutex_);
    audioBackend_.assign(backendName);
}

void DiagnosticsManager::noteAudioBackendStopped()
{
    std::lock_guard lock(textMutex_);
    audioBackend_.clear();
}

void DiagnosticsManager::noteError(std::string_view subsystem, std::string_view message)
{
    std::lock_guard lock(textMutex_);
    lastErrorSubsystem_.assign(subsystem);
    lastErrorMessage_.assign(message);

    // Task 017: an error is also an event. One call site keeps the ring and the
    // "last error" field telling the same story - two writers could drift.
    appendEventLocked(subsystem, message);
}

void DiagnosticsManager::noteEvent(std::string_view subsystem, std::string_view message)
{
    std::lock_guard lock(textMutex_);
    appendEventLocked(subsystem, message);
}

std::vector<DiagnosticsManager::DiagnosticEvent> DiagnosticsManager::events() const
{
    std::lock_guard lock(textMutex_);
    return { events_.begin(), events_.end() };
}

DiagnosticsManager::Snapshot DiagnosticsManager::snapshot() const
{
    Snapshot s;
    s.audioBlocks = audioBlocks_.load(std::memory_order_relaxed);
    s.audioFrames = audioFrames_.load(std::memory_order_relaxed);
    s.underruns = underruns_.load(std::memory_order_relaxed);
    s.overruns = overruns_.load(std::memory_order_relaxed);
    s.partialTextEvents = partialTextEvents_.load(std::memory_order_relaxed);
    s.finalTextEvents = finalTextEvents_.load(std::memory_order_relaxed);
    s.reconnects = reconnects_.load(std::memory_order_relaxed);
    s.ndiErrors = ndiErrors_.load(std::memory_order_relaxed);
    s.translatedAudioFrames = translatedAudioFrames_.load(std::memory_order_relaxed);
    s.rejectedAudioFrames = rejectedAudioFrames_.load(std::memory_order_relaxed);
    s.translatedAudioDroppedFrames = translatedAudioDroppedFrames_.load(std::memory_order_relaxed);
    s.translationSubmittedFrames = translationSubmittedFrames_.load(std::memory_order_relaxed);
    s.translationGapFrames = translationGapFrames_.load(std::memory_order_relaxed);
    s.translationErrors = translationErrors_.load(std::memory_order_relaxed);
    s.translationFatalErrors = translationFatalErrors_.load(std::memory_order_relaxed);
    s.sampleRate = sampleRate_.load(std::memory_order_relaxed);
    s.bufferFrames = bufferFrames_.load(std::memory_order_relaxed);

    std::lock_guard lock(textMutex_);
    s.audioBackend = audioBackend_;
    s.lastErrorSubsystem = lastErrorSubsystem_;
    s.lastErrorMessage = lastErrorMessage_;
    return s;
}

void DiagnosticsManager::resetForTests() noexcept
{
    audioBlocks_.store(0, std::memory_order_relaxed);
    audioFrames_.store(0, std::memory_order_relaxed);
    underruns_.store(0, std::memory_order_relaxed);
    overruns_.store(0, std::memory_order_relaxed);
    partialTextEvents_.store(0, std::memory_order_relaxed);
    finalTextEvents_.store(0, std::memory_order_relaxed);
    reconnects_.store(0, std::memory_order_relaxed);
    ndiErrors_.store(0, std::memory_order_relaxed);
    translatedAudioFrames_.store(0, std::memory_order_relaxed);
    rejectedAudioFrames_.store(0, std::memory_order_relaxed);
    translatedAudioDroppedFrames_.store(0, std::memory_order_relaxed);
    translationSubmittedFrames_.store(0, std::memory_order_relaxed);
    translationGapFrames_.store(0, std::memory_order_relaxed);
    translationErrors_.store(0, std::memory_order_relaxed);
    translationFatalErrors_.store(0, std::memory_order_relaxed);
    sampleRate_.store(0, std::memory_order_relaxed);
    bufferFrames_.store(0, std::memory_order_relaxed);

    // The lock is taken outside a realtime path on purpose; noexcept here is
    // acceptable because std::mutex::lock only fails on a broken mutex, which is
    // already a fatal programming error.
    std::lock_guard lock(textMutex_);
    audioBackend_.clear();
    lastErrorSubsystem_.clear();
    lastErrorMessage_.clear();
    events_.clear();
    eventSequence_ = 0;
    evictedEvents_.store(0, std::memory_order_relaxed);
}

} // namespace liveai
