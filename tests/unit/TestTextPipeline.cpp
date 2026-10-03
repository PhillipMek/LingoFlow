#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "Translation/TextPipeline.h"

using namespace liveai;
using liveai::translation::TextKind;
using liveai::translation::TextPipeline;
using liveai::translation::TranslationTextEvent;

namespace {

/// Records every emitted event, listener-thread-safe.
class Recorder
{
public:
    void attach(TextPipeline& pipeline)
    {
        pipeline.setListener([this](const TranslationTextEvent& event)
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            events_.push_back(event);
        });
    }

    std::vector<TranslationTextEvent> events() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return events_;
    }

    std::size_t count() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return events_.size();
    }

private:
    mutable std::mutex mutex_;
    std::vector<TranslationTextEvent> events_;
};

} // namespace

TEST_CASE("TextPipeline: partials replace the open line, finals own history",
          "[translation][text][pipeline]")
{
    TextPipeline pipeline;
    Recorder rec;
    rec.attach(pipeline);

    pipeline.ingestPartial("Good");
    auto snap = pipeline.snapshot();
    CHECK(snap.currentLine == "Good");
    CHECK(snap.history.empty());

    // The next snapshot replaces, it does not append: fragment assembly is the
    // backend's job (protocol doc section 8), the pipeline's draft is exactly
    // one line as it currently reads.
    pipeline.ingestPartial("Good evening");
    snap = pipeline.snapshot();
    CHECK(snap.currentLine == "Good evening");
    CHECK(snap.history.empty());

    // The final is authoritative: history gets it, the draft is gone.
    pipeline.ingestFinal("Good evening!");
    snap = pipeline.snapshot();
    CHECK(snap.currentLine.empty());
    REQUIRE(snap.history.size() == 1);
    CHECK(snap.history[0].kind == TextKind::final);
    CHECK(snap.history[0].text == "Good evening!");

    // Every emitted event is typed and numbered; the sequence is monotonic
    // across both kinds and arrival stamps never run backwards.
    const auto events = rec.events();
    REQUIRE(events.size() == 3);
    CHECK(events[0].kind == TextKind::partial);
    CHECK(events[1].kind == TextKind::partial);
    CHECK(events[2].kind == TextKind::final);
    CHECK(events[0].sequence == 1);
    CHECK(events[1].sequence == 2);
    CHECK(events[2].sequence == 3);
    CHECK(events[0].arrivalMs <= events[1].arrivalMs);
    CHECK(events[1].arrivalMs <= events[2].arrivalMs);

    CHECK(pipeline.partialEvents() == 2);
    CHECK(pipeline.finalEvents() == 1);
    CHECK(pipeline.snapshot().lastSequence == 3);
}

TEST_CASE("TextPipeline: a final revision replaces the draft it corrects",
          "[translation][text][pipeline]")
{
    TextPipeline pipeline;

    pipeline.ingestPartial("Translat");
    pipeline.ingestFinal("Translation complete");

    const auto snap = pipeline.snapshot();
    CHECK(snap.currentLine.empty());
    REQUIRE(snap.history.size() == 1);              // the superseded draft is
    CHECK(snap.history[0].text == "Translation complete");   // not a second line
}

TEST_CASE("TextPipeline: empty finals close a line, and are counted when there is none",
          "[translation][text][pipeline]")
{
    TextPipeline pipeline;

    pipeline.ingestPartial("Last words");
    pipeline.ingestFinal("");   // "the line ended, no correction"

    auto snap = pipeline.snapshot();
    CHECK(snap.currentLine.empty());
    REQUIRE(snap.history.size() == 1);
    CHECK(snap.history[0].text == "Last words");
    CHECK(snap.history[0].kind == TextKind::final);

    // An empty final with nothing open is noise - counted, never a fake line.
    pipeline.ingestFinal("");
    CHECK(pipeline.snapshot().history.size() == 1);
    CHECK(pipeline.ignoredEmpty() == 1);
}

TEST_CASE("TextPipeline: two honest layers closing the same line produce one entry",
          "[translation][text][pipeline]")
{
    // The order the fault path can produce: the controller sees the session
    // die and closes the draft (closeOpenLine), and the backend's own
    // closeSession flush then arrives with the same words. Whichever comes
    // first, history carries the line exactly once and the repeat is counted.
    TextPipeline pipeline;
    Recorder rec;
    rec.attach(pipeline);

    pipeline.ingestPartial("the interrupted line");
    pipeline.closeOpenLine();
    pipeline.ingestFinal("the interrupted line");

    const auto snap = pipeline.snapshot();
    REQUIRE(snap.history.size() == 1);
    CHECK(snap.history[0].text == "the interrupted line");
    CHECK(pipeline.ignoredDuplicates() == 1);
    CHECK(rec.count() == 2);   // partial + first final; the duplicate emitted nothing

    // And the reverse arrival order works identically.
    TextPipeline other;
    other.ingestPartial("tail");
    other.ingestFinal("tail");           // normal: final equals snapshot
    other.closeOpenLine();               // draft already closed by the final
    CHECK(other.snapshot().history.size() == 1);
    CHECK(other.ignoredDuplicates() == 0);   // closeOpenLine saw an empty draft,
                                             // nothing to duplicate
}

TEST_CASE("TextPipeline: closeOpenLine finalizes exactly what was on the wire",
          "[translation][text][pipeline]")
{
    TextPipeline pipeline;

    // Nothing open: nothing happens, no event, no fake line.
    pipeline.closeOpenLine();
    CHECK(pipeline.snapshot().history.empty());
    CHECK(pipeline.finalEvents() == 0);

    pipeline.ingestPartial("half a sentence, then the connect");
    pipeline.closeOpenLine();

    const auto snap = pipeline.snapshot();
    CHECK(snap.currentLine.empty());
    REQUIRE(snap.history.size() == 1);
    CHECK(snap.history[0].text == "half a sentence, then the connect");
    CHECK(snap.history[0].kind == TextKind::final);

    // Idempotent.
    pipeline.closeOpenLine();
    CHECK(pipeline.snapshot().history.size() == 1);
}

TEST_CASE("TextPipeline: history is bounded and eviction is counted, never silent",
          "[translation][text][pipeline]")
{
    TextPipeline pipeline(3);   // capacity three lines

    for (int i = 1; i <= 5; ++i)
    {
        pipeline.ingestPartial("line " + std::to_string(i));
        pipeline.ingestFinal("line " + std::to_string(i));
    }

    const auto snap = pipeline.snapshot();
    REQUIRE(snap.history.size() == 3);
    CHECK(snap.history[0].text == "line 3");   // the two oldest are gone
    CHECK(snap.history[2].text == "line 5");
    CHECK(pipeline.evictedLines() == 2);

    // recentHistory is a tail view of the same bounded truth.
    const auto tail = pipeline.recentHistory(2);
    REQUIRE(tail.size() == 2);
    CHECK(tail[0].text == "line 4");
    CHECK(tail[1].text == "line 5");
    CHECK(pipeline.recentHistory(99).size() == 3);

    // Sequences survived eviction: the counter is the pipeline's age, not the
    // buffer's (UI ordering stays intact when old lines drop off).
    CHECK(snap.history[2].sequence == 10);   // 5 x (partial + final)
    CHECK(snap.history[0].sequence == 6);
}

TEST_CASE("TextPipeline: a never-ending line is capped, counted, and restartable",
          "[translation][text][pipeline]")
{
    // The FAIL criterion "grows unbounded" defense: a provider that streams
    // partials without ever closing a line cannot make this class eat memory
    // without limit. The cap is an emergency path (the OpenAI backend settles
    // lines far below it); crossing it turns the overlong draft into a final.
    TextPipeline pipeline;

    const std::string huge(70u * 1024u, 'a');   // over the 64 KiB ceiling
    pipeline.ingestPartial(huge);

    const auto snap = pipeline.snapshot();
    CHECK(snap.currentLine.empty());
    REQUIRE(snap.history.size() == 1);
    CHECK(snap.history[0].text == huge);
    CHECK(snap.history[0].kind == TextKind::final);

    // The line restarts: a fresh small partial reads as a fresh line.
    pipeline.ingestPartial("next");
    CHECK(pipeline.snapshot().currentLine == "next");
    CHECK(pipeline.snapshot().history.size() == 1);
}

TEST_CASE("TextPipeline: snapshots are copies; reads and writes race safely",
          "[translation][text][pipeline][threads]")
{
    // The contract does not serialize sink callbacks: run two ingesting threads
    // against one pipeline (audio would be none of their business - this class
    // never touches it) and check the invariants the UI depends on.
    TextPipeline pipeline(16);
    Recorder rec;
    rec.attach(pipeline);

    std::atomic<bool> stop { false };

    auto writer = [&](int id)
    {
        for (int i = 0; i < 300 && !stop.load(); ++i)
        {
            pipeline.ingestPartial("w" + std::to_string(id) + "-" + std::to_string(i));
            if (i % 3 == 0)
                pipeline.ingestFinal("w" + std::to_string(id) + "-" + std::to_string(i));
        }
    };

    std::thread a(writer, 1);
    std::thread b(writer, 2);

    // A reader hammering the UI-facing model concurrently.
    std::thread reader([&]
    {
        while (!stop.load())
        {
            const auto snap = pipeline.snapshot();
            CHECK(snap.history.size() <= 16);
        }
    });

    a.join();
    b.join();
    stop.store(true);
    reader.join();

    pipeline.closeOpenLine();   // the survivor of the race, if any

    const auto snap = pipeline.snapshot();
    CHECK(snap.history.size() <= 16);
    CHECK(snap.currentLine.empty());
    CHECK(pipeline.evictedLines() + snap.history.size() <= 601);   // sane ceiling

    // Event identity under concurrency: sequences are unique and strictly
    // increasing in emission order - the listener runs under the lock, so no
    // two events can share a number or overtake each other.
    const auto events = rec.events();
    std::set<std::uint64_t> seen;
    std::uint64_t previous = 0;
    for (const auto& event : events)
    {
        CHECK(event.sequence > previous);
        CHECK(seen.insert(event.sequence).second);
        previous = event.sequence;
    }
    CHECK(events.size() == pipeline.partialEvents() + pipeline.finalEvents()
                          - pipeline.ignoredDuplicates());

    // Everything the recorder saw is also in the counters' arithmetic:
    // partials + finals - ignored duplicates = emitted events.
    CHECK(pipeline.finalEvents() >= 100);   // 2 threads x 100 finals, minus duplicates
}

TEST_CASE("TextPipeline: nameOf covers both kinds", "[translation][text][pipeline]")
{
    CHECK(translation::nameOf(TextKind::partial) == "partial");
    CHECK(translation::nameOf(TextKind::final) == "final");
}
