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

TextPipeline::TextPipeline(std::size_t historyCapacity) noexcept
    : capacity_(historyCapacity > 0 ? historyCapacity : 1)
{
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

// --------------------------------------------------------------------- internals

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

    // Documented: synchronous, under the lock, on the ingesting thread. The
    // listener must therefore be BOUNDED-COST, and "the audio path never
    // passes here" is not the safety argument it pretended to be (code review
    // P1, 2026-10-05): the OpenAI receiver thread is this product's single
    // consumer of BOTH translated audio and translated text, so a listener
    // that blocks stalls audio delivery one level up, no matter where the
    // samples themselves flow. The NDI listener obeys the bound by
    // construction - NdiDispatch enqueues and returns; the SDK runs on the
    // dispatch worker thread (AGENTS.md 6).
    if (listener_)
        listener_(event);

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
