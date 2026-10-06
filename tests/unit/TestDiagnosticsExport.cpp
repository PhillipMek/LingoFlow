#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "App/ApplicationController.h"
#include "App/UiModel.h"
#include "Config/ConfigSchema.h"
#include "Diagnostics/DiagnosticsExport.h"
#include "Utils/Log.h"

using namespace liveai;

namespace {

class QuietLog
{
public:
    QuietLog()
    {
        LogConfig cfg;
        cfg.level = LogLevel::off;
        cfg.writeConsole = false;
        log::configure(cfg);
    }
    ~QuietLog() { log::resetForTests(); }
};

std::string readWhole(const std::filesystem::path& file)
{
    std::ifstream stream(file, std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

class MapStore final : public security::ISecretStore
{
public:
    std::string_view name() const noexcept override { return "Test Credential Store"; }

    security::SecretStatus store(std::string_view identifier, std::string_view secret) override
    {
        items[std::string(identifier)] = std::string(secret);
        return security::SecretStatus::stored;
    }

    std::optional<std::string> load(std::string_view identifier) override
    {
        const auto it = items.find(std::string(identifier));
        return it == items.end() ? std::nullopt : std::optional(it->second);
    }

    security::SecretStatus remove(std::string_view identifier) override
    {
        return items.erase(std::string(identifier)) > 0 ? security::SecretStatus::found
                                                        : security::SecretStatus::notFound;
    }

    std::vector<std::string> identifiers() const override
    {
        std::vector<std::string> out;
        for (const auto& [id, value] : items)
            out.push_back(id);
        return out;
    }

    std::map<std::string, std::string> items;
};

/// Config's own predicate, used as the injected function pointer in the tests -
/// the exact same wiring the controller's export carries.
bool secretShapedKey(std::string_view key)
{
    return config::isSecretFieldName(key);
}

} // namespace

// ============================================================================ ring

TEST_CASE("DiagnosticsManager: the event ring replays the run in order, bounded, and says so",
          "[diagnostics][events]")
{
    QuietLog quiet;
    DiagnosticsManager diag;

    diag.noteEvent("app", "running");
    diag.noteEvent("translation", "session: connected");
    diag.noteError("translation", "lost: connection");   // errors are events too, one call site

    const auto events = diag.events();
    REQUIRE(events.size() == 3);
    CHECK(events[0].subsystem == "app");
    CHECK(events[1].message == "session: connected");
    CHECK(events[2].subsystem == "translation");
    CHECK(events[0].sequence < events[1].sequence);
    CHECK(events[1].sequence < events[2].sequence);
    CHECK_FALSE(events[0].timestamp.empty());
    CHECK(diag.evictedEvents() == 0);

    // Overflow keeps the recent window and announces the truncation.
    for (int i = 0; i < 300; ++i)
        diag.noteEvent("loop", "event " + std::to_string(i));

    const auto window = diag.events();
    CHECK(window.size() == DiagnosticsManager::kMaxEvents);
    CHECK(diag.evictedEvents() == 47);                    // 303 total, 256 kept

    // 47 events gone from the front: the oldest survivor is the 48th loop event
    // (the three narration events were the first ones).
    CHECK(window.front().message == "event 44");
    CHECK(window.back().message == "event 299");
}

// ============================================================================ render

TEST_CASE("DiagnosticsExport: sections, events, flattening, and the redaction that counts itself",
          "[diagnostics][export]")
{
    QuietLog quiet;

    std::vector<diagnostics::ExportSection> sections {
        { "app", { { "state", "running" }, { "multi", "a\nb\r\nc" } } },
        { "security", { { "api_key", "sk-SHOULD-NEVER-SHIP" }, { "key_present", "yes" } } },
    };

    std::vector<DiagnosticsManager::DiagnosticEvent> events {
        { 1, "2026-10-04 10:00:00.000", "app", "running" },
        { 2, "2026-10-04 10:00:01.000", "translation", "session: connected\nsecond line" },
    };

    const auto rendered = diagnostics::renderExport("0.1.0-test", sections, events, 7, &secretShapedKey);

    CHECK(rendered.text.find("app_version=0.1.0-test") != std::string::npos);
    CHECK(rendered.text.find("events_evicted=7") != std::string::npos);
    CHECK(rendered.text.find("[app]") != std::string::npos);
    CHECK(rendered.text.find("state=running") != std::string::npos);
    CHECK(rendered.text.find("multi=a b c") != std::string::npos);   // flattened, visible, honest

    // The secret-shaped key: value gone, marker present, count spoken.
    CHECK(rendered.text.find("sk-SHOULD-NEVER-SHIP") == std::string::npos);
    CHECK(rendered.text.find("api_key=[redacted]") != std::string::npos);
    CHECK(rendered.text.find("key_present=yes") != std::string::npos);   // a key that does not
                                                                         // look secret-shaped
    CHECK(rendered.redactions == 1);

    // Events keep order, sequence and subsystem; their messages flatten too.
    const auto at = rendered.text.find("[events]");
    REQUIRE(at != std::string::npos);
    const auto eventsBlock = rendered.text.substr(at);
    CHECK(eventsBlock.find("1 2026-10-04 10:00:00.000 [app] running") != std::string::npos);
    CHECK(eventsBlock.find("2 2026-10-04 10:00:01.000 [translation] session: connected second line")
          != std::string::npos);
}

TEST_CASE("DiagnosticsExport: without a predicate no redaction happens - and none is claimed",
          "[diagnostics][export]")
{
    QuietLog quiet;

    const auto rendered = diagnostics::renderExport("v", { { "a", { { "api_key", "shown" } } } }, {}, 0,
                                                    nullptr);

    CHECK(rendered.text.find("api_key=shown") != std::string::npos);
    CHECK(rendered.redactions == 0);
}

// ============================================================================ file

TEST_CASE("DiagnosticsExport: the file lands atomically, directories are created, failures are said",
          "[diagnostics][export]")
{
    QuietLog quiet;

    const auto root = std::filesystem::temp_directory_path() / "lingoflow-017-file-test";
    std::filesystem::remove_all(root);

    std::string error;
    const auto file = root / "deep" / "report.txt";

    REQUIRE(diagnostics::writeExportFile(file, "content\n", error));
    CHECK(readWhole(file) == "content\n");

    // Only the named file: no tmp residue survives the rename.
    std::size_t entries = 0;
    for (std::filesystem::directory_iterator it(file.parent_path()), end; it != end; ++it)
        ++entries;
    CHECK(entries == 1);

    // An impossible target is reported, not swallowed: "report.txt" is occupied by
    // a FILE, so a new temp file in it cannot exist... make the parent a file.
    const auto blocked = root / "blocked";
    {
        std::ofstream maker(blocked);
        maker << "x";
    }
    const auto badFile = blocked / "report.txt";
    CHECK_FALSE(diagnostics::writeExportFile(badFile, "text", error));
    CHECK_FALSE(error.empty());

    std::filesystem::remove_all(root);
}

// ============================================================================ controller

TEST_CASE("ApplicationController: the export answers the venue question, and a canary key proves absent",
          "[diagnostics][app][export]")
{
    QuietLog quiet;

    const auto tempDir = std::filesystem::temp_directory_path() / "lingoflow-017-export";
    std::filesystem::remove_all(tempDir);
    std::filesystem::create_directories(tempDir);

    ApplicationController controller;
    controller.setApplicationVersion("0.1.0-unittest");

    std::string loadNote;
    REQUIRE(controller.loadSettings(tempDir / "config.json", loadNote));

    MapStore store;
    controller.setSecretStore(store);

    const std::string canary = "sk-CANARY-MUST-NEVER-APPEAR-IN-EXPORT";
    std::string note;
    REQUIRE(controller.storeApiSecret(canary, note));

    REQUIRE(controller.start());

    std::filesystem::path written;
    REQUIRE(controller.exportDiagnostics(tempDir, written, note));
    CHECK(note.find("diagnostics written") != std::string::npos);
    CHECK(note.find(canary) == std::string::npos);
    CHECK(std::filesystem::exists(written));

    const std::string report = readWhole(written);

    // The venue question, answerable from the file alone:
    CHECK(report.find("state=running") != std::string::npos);
    CHECK(report.find("sample_rate=48000") != std::string::npos);
    CHECK(report.find("pipeline_buffer_delay_ms=") != std::string::npos);
    CHECK(report.find("measured earlier") != std::string::npos);   // honesty kept in the file
    // The 018 accounting: rows with kinds, the in-flight disclaimer, the limitation line.
    CHECK(report.find("[latency]") != std::string::npos);
    CHECK(report.find("estimated_total_(labeled_rows)=") != std::string::npos);
    CHECK(report.find("NOT mouth-to-ear") != std::string::npos);
    CHECK(report.find("limitations=network and model are one combined") != std::string::npos);
    CHECK(report.find("key_present=yes") != std::string::npos);
    CHECK(report.find("Test Credential Store") != std::string::npos);
    CHECK(report.find("session: connected") != std::string::npos);     // the ring is in
    CHECK(report.find("operator stored the API key") != std::string::npos);
    CHECK(report.find("[audio]") != std::string::npos);
    CHECK(report.find("[translation]") != std::string::npos);
    // The paired truth of the instructions field:
    // unreadable from the log alone, a venue report must see right there that
    // the text was never sent. Default config = not set; if it were set the
    // same key would say "ignored - ...".
    CHECK(report.find("instructions_effect=not set") != std::string::npos);
    // P1: the operator's expectation and the provider's
    // auto-detection are two separate facts in the file (docs section 5,
    // R8) - a venue reading the export cannot mistake the expectation for a
    // parameter OpenAI was given. The old combined "languages=en->ru" line,
    // which read exactly like that mistake, must be gone.
    CHECK(report.find("source_expectation=English (en)") != std::string::npos);
    CHECK(report.find("provider_source_detection=automatic") != std::string::npos);
    CHECK(report.find("target_language=Russian (ru)") != std::string::npos);
    CHECK(report.find("languages=en") == std::string::npos);
    CHECK(report.find("[ndi]") != std::string::npos);
    CHECK(report.find("[settings]") != std::string::npos);

    // And the whole point: the secret is nowhere in the file.
    CHECK(report.find(canary) == std::string::npos);
    CHECK(report.find("sk-") == std::string::npos);

    controller.stop();
    std::filesystem::remove_all(tempDir);
}

TEST_CASE("DiagnosticsExport: redactSecretValues erases every held value, counts it, and ignores short ones",
          "[diagnostics][export][security]")
{
    QuietLog quiet;
    const std::string secret = "sk-VALUE-SHAPE-LAST-NET-0123456789";
    std::string text = "instructions=hello " + secret + " and again " + secret
                       + "\nstream_name=" + secret + "\nmodel_hint=gpt-realtime\nshort=ab\n";

    const auto hits = diagnostics::redactSecretValues(text, { secret, "", "ab", std::string(7, 'x') });

    CHECK(hits == 3);                                   // every occurrence, counted, not swallowed
    CHECK(text.find(secret) == std::string::npos);      // nowhere any more
    CHECK(text.find("[redacted]") != std::string::npos);
    CHECK(text.find("model_hint=gpt-realtime") != std::string::npos);   // innocent lines untouched
    CHECK(text.find("short=ab") != std::string::npos);  // below the threshold: no redaction, no corruption
}

TEST_CASE("ApplicationController: a credential pasted into a harmless field does not ride the receipt",
          "[diagnostics][app][export][security]")
{
    QuietLog quiet;
    const auto tempDir = std::filesystem::temp_directory_path() / "lingoflow-021-paste";
    std::filesystem::remove_all(tempDir);
    std::filesystem::create_directories(tempDir);

    ApplicationController controller;
    controller.setApplicationVersion("0.1.0-unittest");

    std::string loadNote;
    REQUIRE(controller.loadSettings(tempDir / "config.json", loadNote));

    MapStore store;
    controller.setSecretStore(store);

    const std::string canary = "sk-CANARY-PASTED-INTO-A-HARMLESS-FIELD";
    std::string note;
    REQUIRE(controller.storeApiSecret(canary, note));

    // The operator's own mistake this net catches: the key pasted
    // into the free-form instructions field and the NDI stream name. Both ship
    // under key names the secret-SHAPED redactor cannot flag - the value-side
    // pass is the only thing standing between the paste and the venue.
    auto candidate = controller.config().current();
    candidate.translation.instructions = "translate calmly. key: " + canary;
    candidate.ndi.streamName = "LingoFlow " + canary;
    REQUIRE(controller.updateSettings(candidate, note));

    REQUIRE(controller.start());

    std::filesystem::path written;
    REQUIRE(controller.exportDiagnostics(tempDir, written, note));
    const std::string report = readWhole(written);

    // The receipt is clean: the value appears nowhere in it.
    CHECK(report.find(canary) == std::string::npos);
    // The fields still say what they held, minus the secret.
    CHECK(report.find("instructions=translate calmly. key: [redacted]") != std::string::npos);
    CHECK(report.find("stream_name=LingoFlow [redacted]") != std::string::npos);
    // Paired honesty: the instructions truth line still ships.
    CHECK(report.find("instructions_effect=ignored") != std::string::npos);
    // And the operator is told that something was erased, not left guessing.
    CHECK(note.find("redacted") != std::string::npos);

    controller.stop();
    std::filesystem::remove_all(tempDir);
}

TEST_CASE("ApplicationController: the export event lands in the ring after the file - next one has it",
          "[diagnostics][app][export]")
{
    QuietLog quiet;

    const auto tempDir = std::filesystem::temp_directory_path() / "lingoflow-017-echo";
    std::filesystem::remove_all(tempDir);
    std::filesystem::create_directories(tempDir);

    ApplicationController controller;
    std::string loadNote;
    REQUIRE(controller.loadSettings(tempDir / "config.json", loadNote));

    std::filesystem::path first;
    std::filesystem::path second;
    std::string note;

    REQUIRE(controller.exportDiagnostics(tempDir, first, note));
    REQUIRE(controller.exportDiagnostics(tempDir, second, note));

    // The first export cannot contain news of its own completion; the second can.
    CHECK(readWhole(first).find("export written") == std::string::npos);
    CHECK(readWhole(second).find("export written") != std::string::npos);

    std::filesystem::remove_all(tempDir);
}

TEST_CASE("ApplicationController: a write that cannot happen fails with a reason, not a detour",
          "[diagnostics][app][export]")
{
    QuietLog quiet;

    const auto tempDir = std::filesystem::temp_directory_path() / "lingoflow-017-fail";
    std::filesystem::remove_all(tempDir);

    // The "directory" is an existing regular FILE: nothing can be created inside it.
    std::filesystem::create_directories(tempDir);
    const auto blocked = tempDir / "blocked";
    {
        std::ofstream maker(blocked);
        maker << "x";
    }

    ApplicationController controller;
    std::filesystem::path written;
    std::string note;

    CHECK_FALSE(controller.exportDiagnostics(blocked, written, note));
    CHECK(note.find("diagnostics export failed") != std::string::npos);
}

// ============================================================================ latency source

TEST_CASE("UiModel: one latency arithmetic, live first, settings honestly labelled",
          "[app][ui][model][latency]")
{
    QuietLog quiet;
    ApplicationController controller;

    auto cfg = controller.config().current();
    cfg.translation.jitterBufferMs = 100;
    std::string note;
    REQUIRE(controller.updateSettings(cfg, note));

    // Stopped: the estimate answers from the settings and says so.
    auto estimate = estimateBufferDelay(controller.engine(), controller.config().current());
    CHECK(estimate.blockMs == 10);                    // 480 frames at 48 kHz
    CHECK(estimate.totalMs == 120);                   // 10 + 10 + 100
    CHECK_FALSE(estimate.fromEngine);
    CHECK(estimate.source == "settings (not running)");

    REQUIRE(controller.start());

    // Running: the engine's own geometry answers, labelled live.
    estimate = estimateBufferDelay(controller.engine(), controller.config().current());
    CHECK(estimate.fromEngine);
    CHECK(estimate.blockMs == 10);
    CHECK(estimate.totalMs == 10 + 10 + controller.engine().jitterBufferMs());
    CHECK(estimate.source == "live pipeline");

    controller.stop();
}
