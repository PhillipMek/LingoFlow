#pragma once
//
// TextPipeline - the typed text model of task 013: what the contract calls
// "partial/final text" strings become events with a sequence number and an
// arrival timestamp, owned by a bounded pipeline that also keeps the history
// the UI (task 014) and the subtitle transport (task 016) read.
//
// Boundary rules (AGENTS.md 7, SPEC "Text"):
//   * Protocol-neutral vocabulary. A backend decides what a "line" is and
//     hands the pipeline whole-line snapshots (the contract's partial/final
//     pair says exactly that: partial = the in-progress line as it currently
//     reads, final = the authoritative text). Nothing here concatenates
//     provider fragments or knows an event name - the OpenAI backend assembles
//     its append-only deltas into snapshots inside itself (task 009's file),
//     which is where protocol knowledge belongs.
//   * Text never blocks audio (SPEC "Text", the task's FAIL criterion): the
//     pipeline is touched only by backend worker threads and readers (UI),
//     never by the audio callback. Its mutex is never held across anything
//     that can block on I/O, and the listener contract below is what keeps it
//     that way.
//   * Bounded by construction: the history is a fixed-capacity ring of final
//     lines (evictions are counted, never silent), and the pipeline stores at
//     most one open line. A provider that streamed for six hours cannot make
//     this grow past capacity * text length.
//   * "No raw API dependency in UI": the UI-facing model is Snapshot below -
//     typed events with the product's own sequence and arrival clock. The
//     provider's elapsed_ms is deliberately absent: protocol doc section 8
//     says it is alignment metadata that may repeat, so it must not key state
//     here; a future alignment feature reads it at the backend, not above it.
//
// Threading: ingest*() may be called concurrently from any number of backend
// threads (the contract does not serialize sink callbacks). snapshot() and the
// counters are safe from any thread. The listener fires synchronously, on the
// ingesting thread, while the pipeline's lock is held - it must be
// bounded-cost. "Audio never traverses this class" is NOT the safety argument
// it once claimed (code review P1, 2026-10-05): the OpenAI receiver thread
// consumes both translated audio and translated text from one stream, so a
// listener that blocks here stalls that thread and starves the jitter buffer
// downstream. Every product listener honours the bound: the controller's use
// is a counter plus an NDI publish, and NDI publish only enqueues behind its
// own dispatch worker (NDI/NdiDispatch.h) - no network call is ever made on
// the ingesting thread.
//
// Sequence: one monotonic counter per pipeline, stamped on every event the
// pipeline actually emits (duplicates and empty finals it ignores consume no
// number). It gives receivers - UI history position, NDI frame ordering across
// reconnects - a product-owned identity that survives provider changes.
//
// Arrival timestamps: milliseconds since pipeline construction from
// steady_clock. Display/alignment metadata only; nothing in the pipeline keys
// on the clock (settle decisions live in the backend, whose cadence thread is
// where silence is observable).

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace liveai {
namespace translation {

enum class TextKind
{
    partial = 0, ///< the current line as it reads so far; a newer partial replaces it
    final        ///< authoritative: this line is complete; it enters history
};

std::string_view nameOf(TextKind kind) noexcept;

/// One typed text event. `sequence` and `arrivalMs` are assigned here, not by
/// any provider; `text` is a whole line as delivered by the backend.
struct TranslationTextEvent
{
    TextKind kind = TextKind::partial;
    std::string text;
    std::uint64_t sequence = 0;   ///< monotonic per pipeline, starts at 1
    std::uint64_t arrivalMs = 0;  ///< ms since pipeline construction (steady clock)

    friend constexpr bool operator==(const TranslationTextEvent&,
                                     const TranslationTextEvent&) = default;
};

/// The bounded text model. One instance per application, fed by the sink
/// (ApplicationController), read by the UI and forwarded to NDI by a listener.
class TextPipeline final
{
public:
    /// Default final lines kept for the UI. A product choice, not a measurement
    /// (AGENTS.md 19): ~20 minutes of continuous interpreting at a few seconds
    /// per line; the eviction counter tells the operator when it was not enough.
    inline static constexpr std::size_t kDefaultHistoryCapacity = 128;

    using Listener = std::function<void(const TranslationTextEvent& event)>;

    explicit TextPipeline(std::size_t historyCapacity = kDefaultHistoryCapacity) noexcept;

    /// Fires for every emitted event, synchronously on the ingesting thread
    /// (see the header: must not block). Set before session start; changing it
    /// while events flow is not supported and not needed - the controller owns
    /// this instance for its whole lifetime.
    void setListener(Listener listener) noexcept;

    // ------------------------------------------------------------- ingestion
    // Called from backend worker threads via the sink. Ignored inputs are
    // counted, never swallowed silently into nothing observable.

    /// The current line, as it reads now. Replaces the open draft; it does not
    /// touch history. (Backends deliver whole-line snapshots - assembling
    /// provider fragments into a line is backend work, not pipeline work.)
    void ingestPartial(std::string_view text);

    /// The authoritative text of the line that was open. Rules, in order:
    ///   * empty text: closes the open draft if there is one (a "line ended,
    ///     no correction" final), otherwise ignored and counted;
    ///   * equals the just-closed history line while no draft is open: a
    ///     duplicate (a session death the controller already closed, then the
    ///     backend's own close flush arriving with the same words) - ignored
    ///     and counted, history never repeats a line because two layers both
    ///     did the honest thing;
    ///   * otherwise: final; replaces the draft as authoritative, enters
    ///     history, evicts the oldest line when the capacity is reached.
    void ingestFinal(std::string_view text);

    /// Session boundary: the open draft, if any, becomes a final line. When a
    /// session dies or closes mid-sentence the audience is owed the words the
    /// translator actually got: the text exists, it is what the last partial
    /// said, and a new session replaces the draft silently losing it is the
    /// alternative. Idempotent: with no draft open it does nothing.
    void closeOpenLine();

    /// Note there is deliberately no beginSession()/reset for sessions: history
    /// and sequence are application-level (the operator's subtitles do not
    /// forget the first session when the second opens), and a reopened session
    /// simply writes new lines over a draft that closeOpenLine() already
    /// settled. What is per-session lives in the backends (contract rule 6).

    // ----------------------------------------------------------------- reads

    struct Snapshot
    {
        std::string currentLine;                   ///< the open draft, empty if none
        std::vector<TranslationTextEvent> history; ///< oldest first, capacity-bounded
        std::uint64_t lastSequence = 0;            ///< sequence of the newest emitted event
    };

    Snapshot snapshot() const;

    /// Last `count` history entries, oldest first. A convenience view for the
    /// UI when it wants a shorter tail than the full buffer.
    std::vector<TranslationTextEvent> recentHistory(std::size_t count) const;

    // -------------------------------------------------------------- counters
    std::uint64_t partialEvents() const noexcept;      ///< emitted partials
    std::uint64_t finalEvents() const noexcept;        ///< emitted finals (each is one history line)
    std::uint64_t evictedLines() const noexcept;       ///< history lines dropped by the bound
    std::uint64_t ignoredDuplicates() const noexcept;  ///< duplicate finals skipped
    std::uint64_t ignoredEmpty() const noexcept;       ///< empty finals with nothing to close

private:
    /// Stamps the next event, updates the counters and fires the listener.
    /// All callers must hold mutex_.
    TranslationTextEvent stampLocked(TextKind kind, std::string text);

    /// Appends a final line to the bounded history (evicting and counting the
    /// oldest when the capacity is reached). All callers must hold mutex_.
    void pushHistoryLocked(std::string text);

    /// Drops the oldest lines until the history fits its capacity.
    void trimHistoryLocked() noexcept;

    const std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();

    mutable std::mutex mutex_;
    Listener listener_;

    std::string draft_;
    std::deque<TranslationTextEvent> history_;
    std::size_t capacity_;
    std::uint64_t sequence_ = 0;   ///< guarded by mutex_

    // Relaxed counters: written under mutex_ with the state they describe, read
    // lock-free for diagnostics. They never carry ordering meaning.
    std::atomic<std::uint64_t> partialEvents_{ 0 };
    std::atomic<std::uint64_t> finalEvents_{ 0 };
    std::atomic<std::uint64_t> evictedLines_{ 0 };
    std::atomic<std::uint64_t> ignoredDuplicates_{ 0 };
    std::atomic<std::uint64_t> ignoredEmpty_{ 0 };
};

} // namespace translation
} // namespace liveai
