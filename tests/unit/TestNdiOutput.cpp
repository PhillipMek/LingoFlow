#include <catch2/catch_test_macros.hpp>

#include <string>

#include "NDI/NdiTimedText.h"

using namespace liveai;

namespace {

ndi::SubtitleFrame frameOf(std::string text, bool final = false, long long sequence = 1)
{
    ndi::SubtitleFrame frame;
    frame.text = std::move(text);
    frame.final = final;
    frame.sequence = sequence;
    return frame;
}

} // namespace

TEST_CASE("NdiTimedText: one root, no prolog, and the format is the standard we cite",
          "[ndi][timedtext]")
{
    const std::string doc = ndi::buildTimedTextDocument(frameOf("Hello", false, 42));

    CHECK(doc.rfind("<tt ", 0) == 0);                       // single root element
    CHECK(doc.rfind("<?xml", 0) != 0);                      // the no-prolog rule
    CHECK(doc.find("http://www.w3.org/ns/ttml") != std::string::npos);   // TTML1 namespace
    CHECK(doc.find("seq-42") != std::string::npos);         // the pipeline's own identity
    CHECK(doc.find(">Hello</p>") != std::string::npos);
    CHECK(doc.find("</tt>") == doc.size() - 5);

    // No invented wire fields: no language we do not know, no fake timing, no
    // private attributes beyond the standard xml:id.
    CHECK(doc.find("xml:lang") == std::string::npos);
    CHECK(doc.find("begin=") == std::string::npos);
    CHECK(doc.find("final") == std::string::npos);
}

TEST_CASE("NdiTimedText: escaping is total for text content, forbidden characters are removed",
          "[ndi][timedtext]")
{
    CHECK(ndi::buildTimedTextDocument(frameOf("a & b < c > d")).find("a &amp; b &lt; c &gt; d") !=
          std::string::npos);

    // XML 1.0 forbids C0 controls (except tab/newline/CR) outright; a document a
    // receiver cannot parse would be worse than a space, so they become spaces.
    const std::string dirty = std::string("x") + static_cast<char>(0x01) + "y" +
                              static_cast<char>(0x7F) + "z";
    const std::string clean = ndi::buildTimedTextDocument(frameOf(dirty));
    CHECK(clean.find(">x y z<") != std::string::npos);

    // Newline and tab are legal XML content and stay untouched.
    CHECK(ndi::buildTimedTextDocument(frameOf("a\nb\tc")).find(">a\nb\tc<") != std::string::npos);

    // Multi-byte UTF-8 travels as bytes >= 0x80: Cyrillic captions survive whole.
    CHECK(ndi::buildTimedTextDocument(frameOf("Привет мир")).find("Привет мир") !=
          std::string::npos);
}

TEST_CASE("NdiTimedText: sequence is carried honestly, negatives clamp to the first id",
          "[ndi][timedtext]")
{
    CHECK(ndi::buildTimedTextDocument(frameOf("t", true, 7)).find("seq-7") != std::string::npos);
    CHECK(ndi::buildTimedTextDocument(frameOf("t", false, -3)).find("seq-0") != std::string::npos);
}

#ifdef LINGOFLOW_TESTS_WITH_NDI
#define WIN32_LEAN_AND_MEAN 1
#include <windows.h>

#include "NDI/Real/NdiRuntime.h"
#include "NDI/Real/NdiTimedTextOutput.h"

TEST_CASE("NdiTimedTextOutput: the lifecycle is testable against the machine's runtime",
          "[ndi][real]")
{
    ndi::real::NdiTimedTextOutput output;

    // The disabled-by-default state is what a stopped application must show.
    CHECK(output.state() == ndi::OutputState::disabled);

    std::string error;

    // Publishing before start is refused and SAID - never accepted into a void.
    CHECK_FALSE(output.publish(frameOf("early"), error));
    CHECK(error.find("not started") != std::string::npos);

    if (!ndi::real::NdiRuntime::instance().available())
    {
        // No runtime on this machine: the honest branch. The product rule
        // "NDI failure does not stop the show" is proven by start() simply
        // answering false with an actionable sentence - nothing crashed,
        // nothing was faked.
        CHECK_FALSE(output.start("lingoflow-test-unavailable", error));
        CHECK(error.find("runtime") != std::string::npos);
        CHECK(output.state() == ndi::OutputState::disabled);
        return;
    }

    // The live branch: a uniquely named sender, real NDIlib calls.
    const std::string name = "lingoflow-016-unit-" + std::to_string(::GetCurrentProcessId());

    INFO("start error: " + error);
    REQUIRE(output.start(name, error));
    CHECK(output.state() == ndi::OutputState::ready);   // no consumer here: not "publishing"

    // A second start without stop is refused with its reason, not silently
    // restarted.
    CHECK_FALSE(output.start(name + "-again", error));
    CHECK(error.find("already running") != std::string::npos);

    // Captions submit cleanly; publish returns true (the void SDK call, the
    // header explains why that is the honest answer).
    CHECK(output.publish(frameOf("тест", false, 1), error));
    CHECK(output.publish(frameOf("тест", true, 2), error));
    CHECK(output.publishedFrames() == 2);
    CHECK(output.state() == ndi::OutputState::ready);   // still no consumer connected

    // Empty caption: accepted as a no-op, not counted as a failure.
    CHECK(output.publish(frameOf(""), error));
    CHECK(output.publishedFrames() == 2);

    output.stop();
    CHECK(output.state() == ndi::OutputState::disabled);
    CHECK(output.publishedFrames() == 2);   // the run's history, kept like every counter

    // And the sender can be raised again after stop (the operator's Stop+Start) -
    // a fresh start counts from zero, as the header documents.
    INFO("restart error: " + error);
    REQUIRE(output.start(name, error));
    CHECK(output.publishedFrames() == 0);
    output.stop();
}

#endif // LINGOFLOW_TESTS_WITH_NDI
