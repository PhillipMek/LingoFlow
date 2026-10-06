#pragma once
//
// TextPipeline - the typed text model: what the contract calls
// "partial/final text" strings become events with a sequence number and an
// arrival timestamp, owned by a bounded pipeline that also keeps the history
// the UI and the subtitle transport read.
//
// Boundary rules (the project rules, spec "Text"):
//   * Protocol-neutral vocabulary. A backend decides what a "line" is and
//     hands the pipeline whole-line snapshots (the contract's partial/final
//     pair says exactly that: partial = the in-progress line as it currently
//     reads, final = the authoritative text). Nothing here concatenates
//     provider fragments or knows an event name - the OpenAI backend assembles
//     its append-only deltas into snapshots inside itself (the backend's file),
//     which is where protocol knowledge belongs.
//   * Text never blocks audio (spec "Text", the task's FAIL criterion): the
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
// counters are safe from any thread. The listener is NOT run here any more
//: firing user callbacks under mutex_ is one
// future listener away from the classic deadlock (pipeline lock -> listener ->
// anything -> pipeline lock), and the P1 lesson stood beside it - the OpenAI
// receiver thread consumes both translated audio and translated text, so even
// a "bounded-cost" listener had no business running there. The shape now is
// the NdiDispatch pattern this file used to rely on one level down: ingest
// builds and QUEUES the event under the lock and returns; one dispatch worker
// pops and fires the listener without holding mutex_. Consequences, each
// pinned by tests: delivery order equals ingestion order (FIFO, single
// consumer), the listener may call back into the pipeline (snapshot reads,
// counters, even ingest) without deadlock, a wedged listener stalls subtitles
// only - the queue is bounded, the OLDEST pending event is dropped under
// pressure (a stale draft is the cheapest thing here) and counted in
// droppedEvents(), and ingest never blocks on the listener. waitForDispatch()
// is the deterministic barrier for shutdown and tests; stopDispatch() drains
// and joins (the destructor calls it); after the stop, ingest still updates
// state, history and counters but queues nothing.
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
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
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
    ///: ~20 minutes of continuous interpreting at a few seconds
    /// per line; the eviction counter tells the operator when it was not enough.
    inline static constexpr std::size_t kDefaultHistoryCapacity = 128;

    /// Bound of the outgoing dispatch queue. Subtitles arrive a few per second;
    /// 256 pending events means the listener has been wedged for minutes, at
    /// which point the queue is doing its drop-oldest job, not its transport
    /// one. Product choice, mirrors NdiDispatch's queue (the project rules: a text
    /// outage must never become a stall on the threads that ingest).
    inline static constexpr std::size_t kDefaultDispatchQueueCapacity = 256;

    using Listener = std::function<void(const TranslationTextEvent& event)>;

    /// The dispatch worker starts here, so a pipeline that exists can deliver
    /// as soon as a listener does. Throws only if the OS refuses the one
    /// thread (an application that cannot start any worker cannot run the
    /// network chain either; there is no honest way to swallow it - the project rules
    /// 19).
    explicit TextPipeline(std::size_t historyCapacity = kDefaultHistoryCapacity,
                          std::size_t dispatchQueueCapacity = kDefaultDispatchQueueCapacity);

    /// Stops the dispatch worker (drain, then join). The listener may
    /// reference application members, so the join must complete before the
    /// owner goes away.
    ~TextPipeline();

    TextPipeline(const TextPipeline&) = delete;
    TextPipeline& operator=(const TextPipeline&) = delete;

    /// Set before session start; changing it while events flow is not
    /// supported and not needed - the controller owns this instance for its
    /// whole lifetime. The listener fires on the dispatch worker WITHOUT the
    /// pipeline lock held (header): it may call back into the pipeline's
    /// reads and ingest with no deadlock; it must not throw, and it must not
    /// call waitForDispatch() or stopDispatch() (its own delivery is what
    /// those wait for).
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

    /// Events the dispatch worker has finished with (listener run or, if none
    /// is attached, deliberately skipped - counting them keeps the
    /// waitForDispatch arithmetic "queued == delivered + dropped + queued").
    std::uint64_t deliveredEvents() const noexcept;
    std::uint64_t droppedEvents() const noexcept;

    // -------------------------------------------------------------- dispatch
    // (the threading contract is the header comment of this class)

    /// Deterministic barrier: returns once every event ingested before this
    /// call has been delivered to the listener or dropped by the bound - or
    /// the dispatch worker has been stopped. A read-only wait (mutable
    /// barrier machinery), safe to call through the pipeline's const face on
    /// the controller. Control/test threads only, never from the listener
    /// (the listener's own delivery is what this waits on).
    void waitForDispatch() const;

    /// Drains the queue (delivering in order), then joins the worker.
    /// Idempotent; the destructor calls it. Afterwards ingest() still updates
    /// state, history and counters but queues nothing: late events must not
    /// be delivered behind the objects their listener references.
    void stopDispatch() noexcept;

private:
    /// Stamps the next event, updates the counters and queues it for the
    /// dispatch worker (never fires anything itself - see the header). All
    /// callers must hold mutex_.
    TranslationTextEvent stampLocked(TextKind kind, std::string text);

    /// Appends a final line to the bounded history (evicting and counting the
    /// oldest when the capacity is reached). All callers must hold mutex_.
    void pushHistoryLocked(std::string text);

    /// Drops the oldest lines until the history fits its capacity.
    void trimHistoryLocked() noexcept;

    /// The dispatch worker's body: pop under the lock, listener WITHOUT it.
    void dispatchLoop();

    const std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();

    mutable std::mutex mutex_;
    Listener listener_;
    std::atomic<bool> dispatchStopped_{ false };  ///< guarded by mutex_ for writes

    std::string draft_;
    std::deque<TranslationTextEvent> history_;
    std::size_t capacity_;
    std::uint64_t sequence_ = 0;   ///< guarded by mutex_

    std::thread dispatch_;
    std::condition_variable dispatchCv_;          ///< wakes dispatch_ on queue/stop
    std::deque<TranslationTextEvent> outbox_;     ///< bounded, guarded by mutex_
    std::size_t queueCapacity_;
    std::uint64_t queuedEvents_ = 0;              ///< guarded by mutex_

    // waitForDispatch machinery, deliberately separate from mutex_: waiting
    // for delivery must never reach into the lock the pipeline's own state
    // lives behind. Mutable so the barrier is a const read through the
    // controller's const face of the pipeline.
    mutable std::mutex waitMutex_;
    mutable std::condition_variable waitCv_;

    // Relaxed counters: written under mutex_ with the state they describe, read
    // lock-free for diagnostics. They never carry ordering meaning.
    std::atomic<std::uint64_t> partialEvents_{ 0 };
    std::atomic<std::uint64_t> finalEvents_{ 0 };
    std::atomic<std::uint64_t> evictedLines_{ 0 };
    std::atomic<std::uint64_t> ignoredDuplicates_{ 0 };
    std::atomic<std::uint64_t> ignoredEmpty_{ 0 };
    std::atomic<std::uint64_t> deliveredEvents_{ 0 };   ///< after the listener returned
    std::atomic<std::uint64_t> droppedEvents_{ 0 };     ///< queue-pressure evictions
};

} // namespace translation
} // namespace liveai
