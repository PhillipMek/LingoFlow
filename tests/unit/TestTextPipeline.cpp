#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <condition_variable>
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
            record(event);
        });
    }

    /// Also public for listeners that gate first and record later (the
    /// pressure test wedges the worker before recording).
    void record(const TranslationTextEvent& event)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        events_.push_back(event);
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
    // across both kinds and arrival stamps never run backwards. Delivery is
    // async since an earlier review - the barrier makes the recorder's view
    // complete before the first assertion reads it.
    pipeline.waitForDispatch();
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
    pipeline.waitForDispatch();   // async delivery
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
    // never touches it) and check the invariants the UI depends on. The
    // dispatch queue is deliberately oversized against the ~600 events this
    // test can emit, so the recorder sees every event (no pressure drops by
    // construction) while the producer threads still outrun a mutex+push
    // listener occasionally.
    TextPipeline pipeline(16, 2048);
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

    // Event identity under concurrency. Since An earlier review the listener
    // fires on the pipeline's dispatch worker: the queue is FIFO and drained
    // by that single consumer, so events arrive strictly increasing in their
    // sequence numbers, none shared, none overtaken - the same guarantee the
    // under-lock design gave, now without the lock under the callback.
    pipeline.waitForDispatch();
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

TEST_CASE("TextPipeline: the listener is not under the pipeline lock",
          "[translation][text][dispatch]")
{
    // An earlier review, the regression proof: the old design fired
    // the listener with mutex_ held, so ANY reentrant call was a self-deadlock
    // on a non-recursive std::mutex. The barrier returning at all IS the
    // test - every call below would hang forever on the old shape. The
    // listener reenters reads AND ingest: the empty final with an open draft
    // closes it (one more queued event, consumed on the same worker, no
    // recursion loop because the second pass finds nothing open and is
    // counted as ignored-empty).
    TextPipeline pipeline;
    std::atomic<int> snapshotsFromListener { 0 };
    std::atomic<int> eventsSeen { 0 };

    pipeline.setListener([&](const TranslationTextEvent&)
    {
        eventsSeen.fetch_add(1, std::memory_order_relaxed);
        const auto snap = pipeline.snapshot();               // would deadlock once
        pipeline.recentHistory(4);                           // ... every one of these
        pipeline.partialEvents();
        if (!snap.currentLine.empty() || !snap.history.empty())
            snapshotsFromListener.fetch_add(1, std::memory_order_relaxed);
        pipeline.ingestFinal("");
    });

    pipeline.ingestPartial("hello");
    pipeline.waitForDispatch();

    CHECK(snapshotsFromListener.load() >= 1);                // the listener SAW state
    CHECK(eventsSeen.load() >= 2);                           // partial, then its own final
    CHECK(pipeline.ignoredEmpty() >= 1);                     // the terminating recursion step
    CHECK(pipeline.snapshot().history.size() == 1);          // and history stayed sane
    CHECK(pipeline.droppedEvents() == 0);
}

TEST_CASE("TextPipeline: dispatch is FIFO - delivery order equals ingestion order",
          "[translation][text][dispatch]")
{
    TextPipeline pipeline;
    Recorder rec;
    rec.attach(pipeline);

    for (int i = 0; i < 10; ++i)
    {
        pipeline.ingestPartial("line " + std::to_string(i));
        pipeline.ingestFinal("line " + std::to_string(i));
    }

    pipeline.waitForDispatch();
    const auto events = rec.events();

    REQUIRE(events.size() == 20);
    for (std::size_t i = 0; i < events.size(); ++i)
        CHECK(events[i].sequence == i + 1);                  // exactly 1..20, in order
    CHECK(pipeline.deliveredEvents() == 20);
    CHECK(pipeline.droppedEvents() == 0);
}

TEST_CASE("TextPipeline: queue pressure drops the OLDEST pending event, counted",
          "[translation][text][dispatch]")
{
    // The bound that keeps "text never stalls anything" structural: while the
    // listener is wedged mid-delivery of event 1, the queue of capacity 2
    // accepts events 2 and 3 and evicts the stale 2 when 4 arrives. After the
    // wedge clears the audience's line 4 still comes (a newest event can only
    // die if the wedge outlasts the whole queue - counted either way).
    TextPipeline pipeline(8, 2);                              // history 8, queue 2
    Recorder rec;
    std::mutex gateMutex;
    std::condition_variable gateCv;
    bool inListener = false;
    bool gateOpen = false;

    pipeline.setListener([&](const TranslationTextEvent& event)
    {
        {
            const std::lock_guard<std::mutex> lock(gateMutex);
            inListener = true;
        }
        gateCv.notify_all();
        std::unique_lock<std::mutex> lock(gateMutex);
        gateCv.wait(lock, [&] { return gateOpen; });          // wedge the worker
        rec.record(event);                                     // only after release
    });

    pipeline.ingestPartial("one");                             // seq 1, popped, wedged
    {
        std::unique_lock<std::mutex> lock(gateMutex);
        gateCv.wait(lock, [&] { return inListener; });         // deterministic wedge
    }

    pipeline.ingestPartial("two");                             // queued
    pipeline.ingestPartial("three");                           // queued (full)
    pipeline.ingestPartial("four");                            // evicts "two"

    {
        const std::lock_guard<std::mutex> lock(gateMutex);
        gateOpen = true;
    }
    gateCv.notify_all();

    pipeline.waitForDispatch();

    const auto events = rec.events();
    REQUIRE(events.size() == 3);                               // 1 (in flight), 3, 4
    CHECK(events[0].sequence == 1);
    CHECK(events[1].text == "three");
    CHECK(events[2].text == "four");                           // the fresh line survived
    CHECK(pipeline.droppedEvents() == 1);                      // the stale draft did not
    CHECK(pipeline.partialEvents() == 4);                      // the STATE still saw all four
}

TEST_CASE("TextPipeline: stopDispatch drains in order, then ingest queues nothing",
          "[translation][text][dispatch]")
{
    TextPipeline pipeline;
    Recorder rec;
    rec.attach(pipeline);

    for (int i = 0; i < 20; ++i)
        pipeline.ingestFinal("drained " + std::to_string(i));

    pipeline.stopDispatch();          // blocking: delivered everything, joined

    CHECK(rec.count() == 20);
    CHECK(pipeline.deliveredEvents() == 20);
    const auto events = rec.events();
    for (std::size_t i = 0; i < events.size(); ++i)
        CHECK(events[i].sequence == i + 1);                   // drain kept the order

    // After the stop the pipeline keeps telling the truth about state - it
    // simply stops promising delivery, because the objects the listener
    // references are on their way out.
    pipeline.ingestFinal("after stop");
    CHECK(pipeline.snapshot().history.size() == 21);
    CHECK(pipeline.finalEvents() == 21);
    CHECK(pipeline.deliveredEvents() == 20);
    CHECK(rec.count() == 20);

    pipeline.stopDispatch();          // idempotent
    pipeline.waitForDispatch();       // and never hangs after a stop
}
