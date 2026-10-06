// getenv is used here only to detect the cross-process marker; the deprecation
// is opted out the same documented way the probe tools do (it is a test, not
// shipped code - and the variable we look for is our own).
#define _CRT_SECURE_NO_WARNINGS 1

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#include <windows.h>
#endif

#include "App/ApplicationController.h"
#include "Config/ConfigSchema.h"
#include "Security/ChainedSecretStore.h"
#include "Security/WindowsCredentialStore.h"
#include "Utils/Log.h"

using namespace liveai;
using security::SecretStatus;

namespace {

/// An in-memory store with a visible map: the chain and controller tests assert
/// both what reached the store and what did NOT reach anywhere else.
class MapStore final : public security::ISecretStore
{
public:
    explicit MapStore(std::string label, bool writable = true)
        : label_(std::move(label))
        , writable_(writable)
    {
    }

    std::string_view name() const noexcept override { return label_; }

    SecretStatus store(std::string_view identifier, std::string_view secret) override
    {
        if (!writable_)
            return SecretStatus::unavailable;
        items[std::string(identifier)] = std::string(secret);
        return SecretStatus::stored;
    }

    std::optional<std::string> load(std::string_view identifier) override
    {
        const auto it = items.find(std::string(identifier));
        if (it == items.end())
            return std::nullopt;
        return it->second;
    }

    SecretStatus remove(std::string_view identifier) override
    {
        return items.erase(std::string(identifier)) > 0 ? SecretStatus::found
                                                        : SecretStatus::notFound;
    }

    std::vector<std::string> identifiers() const override
    {
        std::vector<std::string> out;
        for (const auto& [id, value] : items)
            out.push_back(id);
        return out;
    }

    std::map<std::string, std::string> items;

private:
    std::string label_;
    bool writable_;
};

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

} // namespace

// ============================================================================ chain

TEST_CASE("ChainedSecretStore: writes the primary, reads primary-first, never rewrites the fallback",
          "[security][chain]")
{
    MapStore primary("Primary");
    MapStore fallback("Fallback");
    security::ChainedSecretStore chain(primary, fallback);

    CHECK(chain.name() == "Primary / fallback: Fallback");

    // A stored value lands in the primary alone.
    REQUIRE(chain.store("k", "primary-value") == SecretStatus::stored);
    CHECK(primary.items.at("k") == "primary-value");
    CHECK(fallback.items.empty());   // the environment is not the product's to rewrite

    // Primary wins when both hold a value.
    fallback.items["k"] = "fallback-value";
    CHECK(chain.load("k") == "primary-value");

    // An empty primary value (a deleted-but-shaped record) still falls through.
    primary.items["k"] = "";
    CHECK(chain.load("k") == "fallback-value");

    primary.items.erase("k");
    CHECK(chain.load("k") == "fallback-value");
    CHECK_FALSE(chain.load("missing").has_value());

    // remove() is primary-only, and identifiers() is the union of names without
    // ever touching values.
    primary.items["k"] = "again";
    REQUIRE(chain.remove("k") == SecretStatus::found);
    CHECK(primary.items.empty());
    CHECK(fallback.items.count("k") == 1);   // the fallback survives - not ours to delete

    fallback.items["only-fallback"] = "v";
    const auto ids = chain.identifiers();
    auto sorted = ids;
    std::sort(sorted.begin(), sorted.end());
    CHECK(sorted == std::vector<std::string>({ "k", "only-fallback" }));
    for (const auto& id : ids)
        CHECK(id.find("fallback-value") == std::string::npos);   // names, never values
}

TEST_CASE("ChainedSecretStore: secretLocation tells secure from convenient",
          "[security][chain]")
{
    MapStore primary("Windows Credential Manager");
    MapStore fallback("Development environment");
    security::ChainedSecretStore chain(primary, fallback);

    using Loc = security::ISecretStore::Location;

    CHECK(chain.secretLocation("k") == Loc::none);

    // An environment-only key is NOT secure storage, and the status line must
    // not say it is: that is the whole reason this query exists.
    fallback.items["k"] = "env-only";
    CHECK(chain.secretLocation("k") == Loc::fallbackOnly);

    REQUIRE(chain.store("k", "written") == SecretStatus::stored);
    CHECK(chain.secretLocation("k") == Loc::primary);

    // The status line names the writable store, not the plumbing diagram.
    CHECK(chain.writableStoreName() == "Windows Credential Manager");
    CHECK(chain.name() == "Windows Credential Manager / fallback: Development environment");

    // A single store can only answer primary-or-none - it never claims a
    // fallback it does not have.
    CHECK(primary.secretLocation("k") == Loc::primary);
    CHECK(primary.secretLocation("zz") == Loc::none);
    CHECK(primary.writableStoreName() == "Windows Credential Manager");
}
TEST_CASE("ChainedSecretStore: an unavailable primary does not fake a store",
          "[security][chain]")
{
    MapStore primary("Read-only primary", /*writable = */ false);
    MapStore fallback("Fallback");
    security::ChainedSecretStore chain(primary, fallback);

    CHECK(chain.store("k", "v") == SecretStatus::unavailable);
    CHECK(fallback.items.empty());   // and it did not quietly succeed somewhere else
}

// ==================================================================== Windows store

TEST_CASE("WindowsCredentialStore: stores, reads back byte-exact and removes",
          "[security][credstore]")
{
    security::WindowsCredentialStore store;

    // A dedicated test identifier: the real app credential (openai_api_key) is
    // never touched by any test, whatever the machine's profile holds.
    const std::string id = "unit-" + std::to_string(::GetCurrentProcessId()) + "-exact";
    store.remove(id);

    // Byte-exactness is the contract: UTF-8 and an embedded NUL travel unharmed
    // (the store copies CredentialBlobSize bytes, nothing else decides).
    std::string value = "sk-Тест";   // "sk-Test" in Cyrillic
    value.append("\0middle-", 8);
    value.append("tail");

    REQUIRE(store.store(id, value) == SecretStatus::stored);

    const auto read = store.load(id);
    REQUIRE(read.has_value());
    CHECK(*read == value);
    CHECK(read->size() == value.size());

    // Presence via names only: the value must never surface in identifiers().
    const auto ids = store.identifiers();
    CHECK(std::find(ids.begin(), ids.end(), id) != ids.end());
    for (const auto& name : ids)
        CHECK(name.find("sk-") == std::string::npos);

    CHECK(store.remove(id) == SecretStatus::found);
    CHECK_FALSE(store.load(id).has_value());
    CHECK(store.remove(id) == SecretStatus::notFound);   // said, not swallowed
}

TEST_CASE("WindowsCredentialStore: the app's credential names do not collide with tests",
          "[security][credstore]")
{
    security::WindowsCredentialStore store;

    // Whatever the developer's machine holds under the production identifier,
    // a test identifier round-trips independently - storing a test value must
    // not overwrite or erase the real one.
    const std::string testId = "unit-" + std::to_string(::GetCurrentProcessId()) + "-isolated";
    const auto before = store.load(security::kOpenAiApiKey);

    REQUIRE(store.store(testId, "isolated") == SecretStatus::stored);
    CHECK(store.load(security::kOpenAiApiKey) == before);   // production value untouched

    store.remove(testId);
    CHECK(store.load(security::kOpenAiApiKey) == before);
}

/// This case has two lives. Inside the normal suite it is a self-contained
/// write+read+delete. The next case filters it by name into a SECOND process -
/// which turns "the store is not this process's memory" from an OS promise
/// into a measured fact, the strongest offline evidence for the established "survives
/// restart". (The actual reboot on the venue machine stays the human
/// checkpoint: that is the REQUIRED one of this task.)
TEST_CASE("Windows credential store: child process writes the marker credential",
          "[security][credstore][child]")
{
    security::WindowsCredentialStore store;
    constexpr std::string_view markerId = "unit-xproc-015";
    constexpr std::string_view markerValue = "cross-process-marker";

    const bool asChild = std::getenv("LINGOFLOW_CRED_CHILD") != nullptr;

    store.remove(markerId);
    REQUIRE(store.store(markerId, markerValue) == SecretStatus::stored);

    if (!asChild)
    {
        // Running as a normal test case: verify in-process and leave nothing behind.
        CHECK(store.load(markerId) == markerValue);
        store.remove(markerId);
    }
    // As the filtered child: the value deliberately STAYS for the parent to
    // read across the process boundary; the parent cleans up.
}

TEST_CASE("WindowsCredentialStore: a value written by another process is readable",
          "[security][credstore]")
{
    security::WindowsCredentialStore store;
    constexpr std::string_view markerId = "unit-xproc-015";
    constexpr std::string_view markerValue = "cross-process-marker";

    store.remove(markerId);   // no stale residue regardless of history

    wchar_t exe[MAX_PATH + 1];
    const DWORD len = ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
    REQUIRE(len > 0);
    REQUIRE(len <= MAX_PATH);

    _wputenv_s(L"LINGOFLOW_CRED_CHILD", L"1");

    // CreateProcessW directly: routed through _wsystem, cmd.exe mangles the
    // nested quoting of exe + a test name that contains spaces. The command
    // line here is what the child actually sees, with no shell interpretation.
    std::wstring cmdline = L"\"" + std::wstring(exe, len) +
                           L"\" \"Windows credential store: child process writes the marker "
                           L"credential\" --reporter compact";

    STARTUPINFOW startup {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process {};

    const bool spawned = ::CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, FALSE, 0,
                                          nullptr, nullptr, &startup, &process);
    const DWORD spawnError = ::GetLastError();   // captured before any later call can rewrite it

    _wputenv_s(L"LINGOFLOW_CRED_CHILD", L"");

    INFO("CreateProcessW failed with OS code " << spawnError);
    REQUIRE(spawned);

    // A child that hangs must not hang the test run: 30 seconds, then it is
    // taken down and the failure is reported, not swallowed.
    const DWORD wait = ::WaitForSingleObject(process.hProcess, 30'000);
    REQUIRE(wait == WAIT_OBJECT_0);

    DWORD exitCode = 1;
    ::GetExitCodeProcess(process.hProcess, &exitCode);
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);

    // Catch2 exits non-zero when a check fails in the child: the exit code is
    // the child's honest verdict.
    CHECK(exitCode == 0);

    const auto value = store.load(markerId);
    REQUIRE(value.has_value());   // read in THIS process what the other one wrote
    CHECK(*value == markerValue);

    store.remove(markerId);
    CHECK_FALSE(store.load(markerId).has_value());
}

// ==================================================================== controller seam

TEST_CASE("ApplicationController: the secret travels field -> store and appears nowhere else",
          "[security][app][credentials]")
{
    QuietLog quiet;
    ApplicationController controller;

    // The default store says "no credentials" as an explicit fact, not as a crash.
    CHECK(controller.secretStoreName() == "Null");
    CHECK_FALSE(controller.hasApiSecret());

    MapStore store("Test Credential Store");
    controller.setSecretStore(store);
    CHECK(controller.secretStoreName() == "Test Credential Store");

    std::string note;

    // "Store" of an empty field is refused, not a silent delete.
    CHECK_FALSE(controller.storeApiSecret("", note));
    CHECK(store.items.empty());

    const std::string canary = "sk-CANARY-MUST-NOT-LEAK";

    REQUIRE(controller.storeApiSecret(canary, note));
    CHECK(store.items.at(std::string(security::kOpenAiApiKey)) == canary);   // byte-exact
    CHECK(controller.hasApiSecret());
    CHECK(note.find(canary) == std::string::npos);                           // the note is clean

    // Settings cannot see it: the config object and its serialized form are both
    // untouched by anything credential-shaped (the project rules, FAIL criterion).
    const auto cfg = controller.config().current();
    CHECK(config::toJsonText(cfg).find(canary) == std::string::npos);
    CHECK(cfg.translation.instructions.find(canary) == std::string::npos);

    // And removing is said in operator words, again without the value.
    controller.removeApiSecret(note);
    CHECK(store.items.empty());
    CHECK_FALSE(controller.hasApiSecret());
    CHECK(note.find(canary) == std::string::npos);
    CHECK(note.find("removed") != std::string::npos);
}

TEST_CASE("ApplicationController: an absent store refusing a write is reported, not swallowed",
          "[security][app][credentials]")
{
    QuietLog quiet;
    ApplicationController controller;

    // The default Null store: it cannot store, and the operator hears exactly that.
    std::string note;
    CHECK_FALSE(controller.storeApiSecret("whatever", note));
    CHECK(note.find("refused") != std::string::npos);
    CHECK(note.find("Null") != std::string::npos);

    controller.removeApiSecret(note);
    CHECK(note.find("no stored API key") != std::string::npos);
}

TEST_CASE("ApplicationController: the log level is live, the log-file sink waits for restart",
          "[security][app][logging]")
{
    QuietLog quiet;
    log::configure([]
    {
        LogConfig cfg;
        cfg.level = LogLevel::info;
        cfg.writeConsole = false;
        return cfg;
    }());

    ApplicationController controller;

    // A settings file in the temp folder: without an attached store the save
    // leg of the funnel honestly fails ("NOT saved"), and this test is about
    // the success note. Tests never write the real %APPDATA% settings.
    const auto tempFile = std::filesystem::temp_directory_path() / "lingoflow-015-logging.json";
    std::filesystem::remove(tempFile);
    std::string loadNote;
    REQUIRE(controller.loadSettings(tempFile, loadNote));

    auto cfg = controller.config().current();

    cfg.diagnostics.logLevel = "debug";
    cfg.diagnostics.writeLogFile = false;

    std::string note;
    REQUIRE(controller.updateSettings(cfg, note));

    // The level took effect right now - the venue run-sheet toggles debug mid-show.
    CHECK(log::config().level == LogLevel::debug);
    CHECK(log::enabled(LogLevel::debug));

    // And the note says honestly which is which.
    CHECK(note.find("log level are live") != std::string::npos);
    CHECK(note.find("application restarts") != std::string::npos);

    // A refused backoff pair changes nothing (the funnel is still the only path).
    auto bad = controller.config().current();
    bad.translation.reconnectMaxBackoffMs = bad.translation.reconnectInitialBackoffMs - 100;
    CHECK_FALSE(controller.updateSettings(bad, note));
    CHECK(note.rfind("settings refused", 0) == 0);
    CHECK(controller.config().current().translation.reconnectMaxBackoffMs
          == cfg.translation.reconnectMaxBackoffMs);

    std::filesystem::remove(tempFile);
}
