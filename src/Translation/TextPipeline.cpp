#include "Translation/TextPipeline.h"

#include <utility>

namespace liveai {
namespace translation {
namespace {

/// The open line a healthy pipeline tolerates from a backend that never closes
/// one. The OpenAI backend settles lines on its own cadence (seconds of
/// speech), so this ceiling is never the normal path: it exists so that the
/// task's FAIL criterion "grows unbounded" cannot be reached by any provider's
/// misbehavior. When it does trigger, the overlong draft becomes a final (the
/// words are real, they simply never ended) and the line restarts empty.
inline constexpr std::size_t kHardDraftCapBytes = 64 * 1024;

} // namespace

std::string_view nameOf(TextKind kind) noexcept
{
    switch (kind)
    {
        case TextKind::partial: return "partial";
        case TextKind::final:   return "final";
    }
    return "partial";
}

TextPipeline::TextPipeline(std::size_t historyCapacity, std::size_t dispatchQueueCapacity)
    : capacity_(historyCapacity > 0 ? historyCapacity : 1)
    , queueCapacity_(dispatchQueueCapacity > 0 ? dispatchQueueCapacity : 1)
{
    dispatch_ = std::thread([this] { dispatchLoop(); });
}

TextPipeline::~TextPipeline()
{
    stopDispatch();
}

void TextPipeline::setListener(Listener listener) noexcept
{
    const std::lock_guard<std::mutex> lock(mutex_);
    listener_ = std::move(listener);
}

void TextPipeline::ingestPartial(std::string_view text)
{
    const std::lock_guard<std::mutex> lock(mutex_);

    draft_.assign(text);

    if (draft_.size() > kHardDraftCapBytes)
    {
        pushHistoryLocked(std::move(draft_));   // clears draft_ after taking the words
        return;
    }

    stampLocked(TextKind::partial, draft_);     // the event is the current line
}

void TextPipeline::ingestFinal(std::string_view text)
{
    const std::lock_guard<std::mutex> lock(mutex_);

    if (text.empty())
    {
        // An empty final closes the open draft with the draft's own words
        // ("the line ended, no correction"); with nothing open it is noise,
        // counted, never silently swallowed into history.
        if (draft_.empty())
        {
            ignoredEmpty_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        pushHistoryLocked(std::move(draft_));
        return;
    }

    if (draft_.empty() && !history_.empty() && history_.back().text == text)
    {
        // The same words finalized twice - the controller's session-boundary
        // close and a backend's own close flush can arrive in either order.
        // History never repeats because two honest layers both did the right
        // thing; the repeat is counted, not hidden.
        ignoredDuplicates_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // The final is authoritative. Whether the draft was one of its snapshots
    // or a revision the provider corrected on the way, the draft is superseded
    // and the final owns the line.
    pushHistoryLocked(std::string(text));
}

void TextPipeline::closeOpenLine()
{
    const std::lock_guard<std::mutex> lock(mutex_);

    if (draft_.empty())
        return;

    // The session is over or lost; the draft's text is the last thing the
    // translator said and the audience is owed it in history. It becomes a
    // final exactly like the normal path, and the duplicate guard in
    // ingestFinal keeps a backend flush of the same words from double-stamping.
    pushHistoryLocked(std::move(draft_));
}

TextPipeline::Snapshot TextPipeline::snapshot() const
{
    const std::lock_guard<std::mutex> lock(mutex_);

    Snapshot s;
    s.currentLine = draft_;
    s.history.assign(history_.begin(), history_.end());
    s.lastSequence = sequence_;
    return s;
}

std::vector<TranslationTextEvent> TextPipeline::recentHistory(std::size_t count) const
{
    const std::lock_guard<std::mutex> lock(mutex_);

    const std::size_t begin = history_.size() > count ? history_.size() - count : 0;
    return { history_.begin() + static_cast<std::ptrdiff_t>(begin), history_.end() };
}

std::uint64_t TextPipeline::partialEvents() const noexcept
{
    return partialEvents_.load(std::memory_order_relaxed);
}

std::uint64_t TextPipeline::finalEvents() const noexcept
{
    return finalEvents_.load(std::memory_order_relaxed);
}

std::uint64_t TextPipeline::evictedLines() const noexcept
{
    return evictedLines_.load(std::memory_order_relaxed);
}

std::uint64_t TextPipeline::ignoredDuplicates() const noexcept
{
    return ignoredDuplicates_.load(std::memory_order_relaxed);
}

std::uint64_t TextPipeline::ignoredEmpty() const noexcept
{
    return ignoredEmpty_.load(std::memory_order_relaxed);
}

std::uint64_t TextPipeline::deliveredEvents() const noexcept
{
    return deliveredEvents_.load(std::memory_order_relaxed);
}

std::uint64_t TextPipeline::droppedEvents() const noexcept
{
    return droppedEvents_.load(std::memory_order_relaxed);
}

void TextPipeline::waitForDispatch() const
{
    std::uint64_t target;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        target = queuedEvents_;
    }

    std::unique_lock<std::mutex> lock(waitMutex_);   // cv::wait relocks - must not be const
    waitCv_.wait(lock, [this, target]
    {
        // Stop releases every waiter: a stopped worker delivers nothing more,
        // and a barrier that hangs on shutdown would be a defect of its own.
        return dispatchStopped_.load(std::memory_order_relaxed)
            || deliveredEvents_.load(std::memory_order_relaxed)
                 + droppedEvents_.load(std::memory_order_relaxed) >= target;
    });
}

void TextPipeline::stopDispatch() noexcept
{
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        dispatchStopped_.store(true, std::memory_order_relaxed);
    }
    dispatchCv_.notify_all();

    {
        const std::lock_guard<std::mutex> lock(waitMutex_);
        waitCv_.notify_all();
    }

    // The worker exits only on stop WITH an empty queue (see dispatchLoop):
    // everything that was queued before the flag gets delivered, in order,
    // before this returns. A caller from inside the listener would join
    // itself - documented against, and the application never does it.
    if (dispatch_.joinable())
        dispatch_.join();
}

// --------------------------------------------------------------------- internals

void TextPipeline::dispatchLoop()
{
    for (;;)
    {
        TranslationTextEvent event;
        Listener listener;
        {
            std::unique_lock<std::mutex> lock(mutex_);   // cv::wait relocks - must not be const
            dispatchCv_.wait(lock, [this]
            {
                return dispatchStopped_.load(std::memory_order_relaxed) || !outbox_.empty();
            });

            if (outbox_.empty())
            {
                // Only a stop can wake us with nothing to do: drained first,
                // then out.
                if (dispatchStopped_.load(std::memory_order_relaxed))
                    return;
                continue;
            }

            event = std::move(outbox_.front());
            outbox_.pop_front();
            listener = listener_;  // copy; the callback below runs WITHOUT mutex_
        }

        if (listener)
            listener(event);

        {
            const std::lock_guard<std::mutex> lock(waitMutex_);
            deliveredEvents_.fetch_add(1, std::memory_order_relaxed);
        }
        waitCv_.notify_all();
    }
}

TranslationTextEvent TextPipeline::stampLocked(TextKind kind, std::string text)
{
    TranslationTextEvent event;
    event.kind = kind;
    event.text = std::move(text);
    event.sequence = ++sequence_;
    event.arrivalMs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_).count());

    if (kind == TextKind::partial)
        partialEvents_.fetch_add(1, std::memory_order_relaxed);
    else
        finalEvents_.fetch_add(1, std::memory_order_relaxed);

    // Queue for the dispatch worker - the listener is NEVER fired here (code
    // review P2, 2026-10-05): this runs with mutex_ held on an ingesting
    // thread, which used to be the documented shape and the deadlock the
    // header describes. Under sustained listener stall the OLDEST pending
    // event is evicted - a draft the audience never saw is worth less than
    // the fresh line already in the queue - and the eviction is counted,
    // never silent. After stopDispatch nothing is queued: state and counters
    // stay honest while the application winds down, but events must not be
    // delivered behind the objects their listener references.
    if (!dispatchStopped_.load(std::memory_order_relaxed))
    {
        if (outbox_.size() >= queueCapacity_)
        {
            outbox_.pop_front();
            droppedEvents_.fetch_add(1, std::memory_order_relaxed);
        }
        outbox_.push_back(event);
        ++queuedEvents_;
        dispatchCv_.notify_one();
    }

    return event;
}

void TextPipeline::pushHistoryLocked(std::string text)
{
    // Whatever draft was open belongs to the words just made final: it is the
    // snapshot this text supersedes. Clear first, then the history owns them.
    draft_.clear();
    history_.push_back(stampLocked(TextKind::final, std::move(text)));
    trimHistoryLocked();
}

void TextPipeline::trimHistoryLocked() noexcept
{
    while (history_.size() > capacity_)
    {
        history_.pop_front();
        evictedLines_.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace translation
} // namespace liveai
