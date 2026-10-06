#include <catch2/catch_test_macros.hpp>

#include <format>
#include <string>
#include <vector>

#include "App/UiModel.h"
#include "Audio/IAudioBackend.h"
#include "Diagnostics/DiagnosticsManager.h"
#include "Utils/Log.h"

using namespace liveai;

namespace {

struct QuietLog
{
    QuietLog()
    {
        LogConfig cfg;
        cfg.level = LogLevel::off;
        cfg.writeConsole = false;
        log::configure(cfg);
    }
    ~QuietLog() { log::resetForTests(); }
};

/// A device that only answers capability questions - everything else is refused
/// in words, because this test never opens it.
class ReportBackend final : public audio::IAudioBackend
{
public:
    std::string_view name() const noexcept override { return "Report Backend"; }
    audio::BackendState state() const noexcept override { return audio::BackendState::opened; }

    bool open(audio::IAudioProcessor&, const audio::DeviceRequest&, std::string& error) override
    {
        error = "not for opening";
        return false;
    }
    bool start(std::string& error) override { error = "not for starting"; return false; }
    bool stop(std::string& error) override { error = "not for stopping"; return false; }
    void close() noexcept override {}

    audio::DeviceCapabilities capabilities() const noexcept override { return caps_; }

    audio::DeviceCapabilities caps_;
};

const LatencyRow* findRow(const std::vector<LatencyRow>& rows, std::string_view component)
{
    for (const auto& row : rows)
    {
        if (row.component == component)
            return &row;
    }
    return nullptr;
}

std::string rowValue(const std::vector<LatencyRow>& rows, std::string_view component)
{
    const auto* row = findRow(rows, component);
    return row != nullptr ? row->value : "<row missing>";
}

} // namespace

TEST_CASE("Latency accounting: every row carries its kind; no source-less numbers",
          "[app][latency][accounting]")
{
    QuietLog quiet;
    ApplicationController controller;
    DiagnosticsManager diag;

    const auto rows = latencyAccounting(controller.engine(), controller.audioBackend(),
                                        diag.snapshot(), controller.config().current());

    REQUIRE_FALSE(rows.empty());

    for (const auto& row : rows)
    {
        INFO(row.component);
        CHECK_FALSE(row.kind.empty());
        CHECK_FALSE(row.value.empty());
    }

    // Stopped: blocks are settings arithmetic, drivers "no answer", nothing is
    // claimed to have been measured.
    const auto* block = findRow(rows, "capture block in");
    REQUIRE(block != nullptr);
    CHECK(block->value == "10 ms");   // the defaults' arithmetic, labeled as such
    CHECK(block->kind == "arithmetic (settings, not running)");

    CHECK(rowValue(rows, "asio input latency").find("not reported") != std::string::npos);
    CHECK(rowValue(rows, "network + model (audio in flight)") == "nothing submitted yet");

    const auto* total = findRow(rows, "estimated total (labeled rows)");
    REQUIRE(total != nullptr);
    CHECK(total->kind.find("NOT mouth-to-ear") != std::string::npos);
    CHECK(total->kind.find("docs/latency-budget.md") != std::string::npos);
    // 0 driver + 10 block + 0 in flight + 120 engine pre-roll + 10 block + 0 driver.
    CHECK(total->value.find(std::format("{:.1f} ms", 10.0 + controller.engine().jitterBufferMs()
                                                        + 10.0)) != std::string::npos);
}

TEST_CASE("Latency accounting: the in-flight row is counters doing arithmetic, clamped and honest",
          "[app][latency][accounting]")
{
    QuietLog quiet;
    ApplicationController controller;
    DiagnosticsManager diag;

    // 9600 frames entered the wire, 4800 came back: 4800 in flight = 100 ms @ 48k.
    diag.countTranslationSubmittedFrames(9600);
    diag.countTranslatedAudioFrames(4800);

    auto rows = latencyAccounting(controller.engine(), controller.audioBackend(), diag.snapshot(),
                                  controller.config().current());
    const auto* flight = findRow(rows, "network + model (audio in flight)");
    REQUIRE(flight != nullptr);
    CHECK(flight->value == "100.0 ms (4800 frames waiting to come back)");
    CHECK(flight->kind.find("live-computed") != std::string::npos);
    CHECK(flight->kind.find("no provider-side timestamp") != std::string::npos);

    // Gap-refused frames never entered the wire, so they do not join the backlog.
    diag.countTranslationGapFrames(2400);
    rows = latencyAccounting(controller.engine(), controller.audioBackend(), diag.snapshot(),
                             controller.config().current());
    CHECK(rowValue(rows, "network + model (audio in flight)")
          == "100.0 ms (4800 frames waiting to come back)");

    // Rejected-and-dropped arrivals have left the path: they count as returned.
    diag.countRejectedAudioFrames(2000);
    diag.countTranslatedAudioDroppedFrames(2800);   // 4800+2000+2800 = 9600 = everything
    rows = latencyAccounting(controller.engine(), controller.audioBackend(), diag.snapshot(),
                             controller.config().current());
    CHECK(rowValue(rows, "network + model (audio in flight)")
          == "0.0 ms (0 frames waiting to come back)");

    // A skew across threads must never render a negative latency: clamp holds.
    diag.countTranslatedAudioFrames(100000);
    rows = latencyAccounting(controller.engine(), controller.audioBackend(), diag.snapshot(),
                             controller.config().current());
    CHECK(rowValue(rows, "network + model (audio in flight)")
          == "0.0 ms (0 frames waiting to come back)");
}

TEST_CASE("Latency accounting: a driver that reports is quoted with its evidence; zero is not speed",
          "[app][latency][accounting]")
{
    QuietLog quiet;
    ApplicationController controller;
    DiagnosticsManager diag;

    ReportBackend backend;
    backend.caps_.inputLatencySamples = 144;    // 3.0 ms at the settings' 48 kHz
    backend.caps_.outputLatencySamples = 288;   // 6.0 ms

    auto rows = latencyAccounting(controller.engine(), &backend, diag.snapshot(),
                                  controller.config().current());

    const auto* in = findRow(rows, "asio input latency");
    REQUIRE(in != nullptr);
    CHECK(in->value == "3.0 ms (144 frames, reported by the driver)");
    CHECK(in->kind == "driver-reported");
    CHECK(rowValue(rows, "asio output latency") == "6.0 ms (288 frames, reported by the driver)");

    // The total now visibly includes the reported numbers.
    const auto* total = findRow(rows, "estimated total (labeled rows)");
    REQUIRE(total != nullptr);
    CHECK(total->value.find(std::format("{:.1f} ms", 3.0 + 6.0 + 10.0 + 10.0
                                        + static_cast<double>(controller.engine().jitterBufferMs())))
          != std::string::npos);

    // The same backend answering zero is rendered "not reported", contributing
    // exactly zero and saying which it is.
    backend.caps_.inputLatencySamples = 0;
    backend.caps_.outputLatencySamples = 0;
    rows = latencyAccounting(controller.engine(), &backend, diag.snapshot(),
                             controller.config().current());
    CHECK(rowValue(rows, "asio input latency") == "not reported by this driver");
    CHECK(findRow(rows, "asio input latency")->kind == "not measured");
}

TEST_CASE("Latency accounting: the panel, the diagnostics surface and the export render one function",
          "[app][latency][accounting][ui]")
{
    QuietLog quiet;
    ApplicationController controller;

    const auto panel = buildOperatorPanel(controller, {});
    const auto diagPanel = buildDiagnosticsPanel(controller);

    // the UI redesign moved the full accounting to the diagnostics surface; the operator
    // panel keeps only the headline sentence. One function still feeds every
    // consumer that shows the breakdown (screen detail, diagnostics window,
    // export), so no two of them can tell two stories.
    CHECK_FALSE(diagPanel.latency.empty());
    CHECK(diagPanel.latency == latencyAccounting(controller.engine(), controller.audioBackend(),
                                                 controller.diagnostics().snapshot(),
                                                 controller.config().current()));
    // The headline still leads with the buffer arithmetic and its disclaimer.
    CHECK(panel.latencySummary.find("NOT included") != std::string::npos);
}
