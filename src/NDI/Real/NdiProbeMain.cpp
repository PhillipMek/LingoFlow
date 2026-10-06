// lingoflow_ndi_probe - NDI transport verification tool.
//
// It runs the SAME runtime-loading path and the SAME document builder as the
// product, so what this tool shows on the network is what the app sends - the
// probe pattern established by the ASIO and OpenAI probes, kept honest by
// sharing code instead of duplicating it.
//
// Modes:
//   list [timeoutMs]                      sources visible on this network
//   recv <namePart> <timeoutMs>           subscribe (metadata-only bandwidth)
//                                         and print every metadata frame received
//   send <name> <seconds>                 publish our TTML captions at 2 Hz
//   selfcheck [timeoutMs]                 send + find + recv inside one process:
//                                         a full NDI round-trip on this machine,
//                                         printing the XML the SDK returns
//
// Exit codes: 0 success, 2 usage/runtime problems, 3 timed out with no data.
//
// The dynamic-load rule (docs/licensing.md) means this exe links no NDI import
// library either: same loader, same story.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN 1
#include <windows.h>
#endif

#include "NDI/NdiTimedText.h"
#include "NDI/Real/NdiRuntime.h"

using namespace liveai;
using ndi::real::NdiRuntime;

namespace {

unsigned long parseNumber(const char* text, unsigned long fallback)
{
    if (text == nullptr || *text == '\0')
        return fallback;

    char* end = nullptr;
    const unsigned long value = std::strtoul(text, &end, 10);
    return (end != nullptr && *end == '\0') ? value : fallback;
}

const NDIlib_v6& requireApi()
{
    const NdiRuntime& runtime = NdiRuntime::instance();
    if (!runtime.available())
    {
        std::printf("NDI runtime unavailable: %s\n", runtime.loadError().c_str());
        std::exit(2);
    }
    return runtime.api();
}

// Discovery ------------------------------------------------------------------

/// First source whose name contains `part`, waiting up to timeoutMs. The strings
/// are copied into statics because the SDK documents the sources array as valid
/// only until the next call or destroy; this tool runs one lookup at a time.
const NDIlib_source_t* findSourceNamed(const std::string& part, uint32_t timeoutMs)
{
    const NDIlib_v6& api = requireApi();

    NDIlib_find_create_t findDesc {};
    findDesc.p_groups = nullptr;

    NDIlib_find_instance_t finder = api.find_create_v2(&findDesc);
    if (finder == nullptr)
    {
        std::printf("NDIlib_find_create_v2 failed\n");
        return nullptr;
    }

    uint32_t count = 0;
    const NDIlib_source_t* sources = nullptr;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

    while (std::chrono::steady_clock::now() < deadline)
    {
        // Two-argument form as declared in Processing.NDI.Find.h: the sources
        // array refreshes on each call; pacing comes from the sleep below.
        sources = api.find_get_current_sources(finder, &count);

        for (uint32_t i = 0; i < count; ++i)
        {
            if (sources[i].p_ndi_name != nullptr &&
                std::strstr(sources[i].p_ndi_name, part.c_str()) != nullptr)
            {
                static std::string keptName;
                static std::string keptUrl;
                static NDIlib_source_t copy;

                keptName = sources[i].p_ndi_name;
                keptUrl = sources[i].p_url_address != nullptr ? sources[i].p_url_address : "";
                copy.p_ndi_name = keptName.c_str();
                copy.p_url_address = keptUrl.c_str();

                api.find_destroy(finder);
                return &copy;
            }
        }

        // Poll gently: NDI's own wait tells us when the list changed.
        api.find_wait_for_sources(finder, 200);
    }

    api.find_destroy(finder);
    return nullptr;
}

void printSources(uint32_t timeoutMs)
{
    const NDIlib_v6& api = requireApi();

    NDIlib_find_create_t findDesc {};
    findDesc.p_groups = nullptr;
    NDIlib_find_instance_t finder = api.find_create_v2(&findDesc);

    std::printf("[probe] finder=%p\n", (void*)finder);
    std::fflush(stdout);

    if (finder == nullptr)
    {
        std::printf("NDIlib_find_create_v2 failed\n");
        std::exit(2);
    }

    std::printf("[probe] waiting for sources (%u ms)...\n", timeoutMs);
    std::fflush(stdout);
    api.find_wait_for_sources(finder, timeoutMs);   // best-effort warm-up

    uint32_t count = 0;
    const NDIlib_source_t* sources = api.find_get_current_sources(finder, &count);

    std::printf("%u NDI source(s) visible:\n", count);
    for (uint32_t i = 0; i < count; ++i)
    {
        std::printf("  [%u] %s  (%s)\n", i, sources[i].p_ndi_name,
                    sources[i].p_url_address != nullptr ? sources[i].p_url_address : "?");
    }

    api.find_destroy(finder);
}

// Receiving ------------------------------------------------------------------

uint32_t receiveMetadata(const NDIlib_source_t& source, uint32_t timeoutMs, uint32_t stopAfter)
{
    const NDIlib_v6& api = requireApi();

    NDIlib_recv_create_v3_t recvDesc {};
    recvDesc.source_to_connect_to = source;
    recvDesc.color_format = NDIlib_recv_color_format_fastest;   // irrelevant for metadata
    recvDesc.bandwidth = NDIlib_recv_bandwidth_metadata_only;   // captions only, no video

    NDIlib_recv_instance_t receiver = api.recv_create_v3(&recvDesc);
    if (receiver == nullptr)
    {
        std::printf("NDIlib_recv_create_v3 failed\n");
        return 0;
    }

    uint32_t received = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

    while (std::chrono::steady_clock::now() < deadline && received < stopAfter)
    {
        NDIlib_video_frame_v2_t video {};
        NDIlib_audio_frame_v3_t audio {};
        NDIlib_metadata_frame_t metadata {};

        const NDIlib_frame_type_e type =
            api.recv_capture_v3(receiver, &video, &audio, &metadata, 200);

        if (type == NDIlib_frame_type_metadata)
        {
            ++received;
            std::printf("--- metadata frame %u (length %d):\n%s\n", received, metadata.length,
                        metadata.p_data != nullptr ? metadata.p_data : "(null)");
            api.recv_free_metadata(receiver, &metadata);
        }
        else if (type == NDIlib_frame_type_error)
        {
            std::printf("recv error: %s\n", metadata.p_data != nullptr ? metadata.p_data : "(none)");
            api.recv_free_metadata(receiver, &metadata);
            break;
        }
    }

    api.recv_destroy(receiver);
    return received;
}

// Sending --------------------------------------------------------------------

NDIlib_send_instance_t createSender(const std::string& name)
{
    const NDIlib_v6& api = requireApi();

    NDIlib_send_create_t createDesc {};
    createDesc.p_ndi_name = name.c_str();
    createDesc.p_groups = nullptr;
    createDesc.clock_video = true;
    createDesc.clock_audio = true;

    NDIlib_send_instance_t sender = api.send_create(&createDesc);
    if (sender == nullptr)
    {
        std::printf("NDIlib_send_create failed for '%s'\n", name.c_str());
        std::exit(2);
    }
    return sender;
}

uint32_t sendCaptions(NDIlib_send_instance_t sender, uint32_t seconds)
{
    const NDIlib_v6& api = requireApi();

    uint32_t sent = 0;

    for (uint32_t tick = 0; tick < seconds * 2; ++tick)
    {
        ndi::SubtitleFrame frame;
        frame.sequence = sent + 1;
        frame.final = tick % 4 == 3;
        frame.text = "LingoFlow NDI test line " + std::to_string(sent + 1) + " - проверка подписей";

        const std::string document = ndi::buildTimedTextDocument(frame);

        NDIlib_metadata_frame_t metadata {};
        metadata.length = static_cast<int>(document.size()) + 1;
        metadata.timecode = NDIlib_send_timecode_synthesize;
        metadata.p_data = const_cast<char*>(document.c_str());

        api.send_send_metadata(sender, &metadata);
        ++sent;

        std::printf("sent caption %u (%zu bytes XML)\n", sent, document.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    return sent;
}

// selfcheck: one process is both producer and consumer of our own captions.
//
// It does NOT rely on discovery: on an isolated dev laptop (no mDNS route, a
// discovery service with nothing to announce) the source list can legitimately
// stay empty - a property of the machine, not of the transport. The sender's
// own URL is readable through send_get_source_name (verified against the 6.3
// headers), so the probe connects to it directly: a real TCP NDI connection
// over loopback, metadata-only bandwidth, carrying the exact document the app
// sends. Discovery stays the fallback, and its failure is reported as a fact.
int selfcheck(uint32_t timeoutMs)
{
    char name[128];
    std::snprintf(name, sizeof(name), "lingoflow-selfcheck-%lu",
#ifdef _WIN32
                  static_cast<unsigned long>(::GetCurrentProcessId()));
#else
                  static_cast<unsigned long>(::getpid()));
#endif

    NDIlib_send_instance_t sender = createSender(name);
    const NDIlib_v6& api = requireApi();

    std::string keptName;
    std::string keptUrl;
    NDIlib_source_t source {};

    if (const NDIlib_source_t* own = api.send_get_source_name(sender);
        own != nullptr && own->p_url_address != nullptr && own->p_url_address[0] != '\0')
    {
        keptName = own->p_ndi_name != nullptr ? own->p_ndi_name : name;
        keptUrl = own->p_url_address;
        source.p_ndi_name = keptName.c_str();
        source.p_url_address = keptUrl.c_str();
        std::printf("selfcheck: sender URL '%s' at %s\n", source.p_ndi_name, source.p_url_address);
    }
    else
    {
        std::printf("selfcheck: sender advertised no URL, falling back to discovery\n");
        const NDIlib_source_t* found = findSourceNamed(name, timeoutMs);
        if (found == nullptr)
        {
            std::printf("selfcheck: source '%s' has neither a URL nor a discovery hit\n", name);
            api.send_destroy(sender);
            return 3;
        }
        source = *found;
    }

    std::atomic<uint32_t> received{ 0 };
    std::thread recvThread(
        [&] { received.store(receiveMetadata(source, timeoutMs, 2), std::memory_order_relaxed); });

    const uint32_t sent = sendCaptions(sender, timeoutMs / 1000 + 1);
    recvThread.join();

    api.send_destroy(sender);

    std::printf("selfcheck: sent %u captions, received %u metadata frame(s)\n", sent,
                received.load(std::memory_order_relaxed));
    return received.load(std::memory_order_relaxed) > 0 ? 0 : 3;
}

} // namespace

int main(int argc, char* argv[])
{
    // Unbuffered stdout: a diagnostic tool that loses lines on an abnormal exit
    // tells its operator nothing. Every printf below is therefore immediate.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    std::printf("[probe] start\n");
    std::fflush(stdout);

    if (argc < 2)
    {
        std::printf("usage:\n"
                    "  lingoflow_ndi_probe list [timeoutMs]\n"
                    "  lingoflow_ndi_probe recv <namePart> <timeoutMs>\n"
                    "  lingoflow_ndi_probe send <name> <seconds>\n"
                    "  lingoflow_ndi_probe selfcheck [timeoutMs]\n");
        return 2;
    }

    const std::string mode = argv[1];

    std::printf("[probe] loading NDI runtime...\n");
    std::fflush(stdout);

    const NdiRuntime& runtime = NdiRuntime::instance();
    const std::string version = runtime.versionText();

    std::printf("[probe] runtime available=%d version=%s error=%s\n",
                runtime.available() ? 1 : 0, version.c_str(), runtime.loadError().c_str());
    std::fflush(stdout);

    if (mode == "list")
    {
        std::printf("[probe] list: creating finder...\n");
        std::fflush(stdout);
        printSources(static_cast<uint32_t>(parseNumber(argc > 2 ? argv[2] : nullptr, 3000)));
        std::printf("[probe] list: done\n");
        std::fflush(stdout);
        return 0;
    }

    if (mode == "recv")
    {
        if (argc < 3)
        {
            std::printf("recv needs <namePart> [timeoutMs]\n");
            return 2;
        }

        const uint32_t timeout = static_cast<uint32_t>(parseNumber(argc > 3 ? argv[3] : nullptr, 10000));

        const NDIlib_source_t* source = findSourceNamed(argv[2], timeout);
        if (source == nullptr)
        {
            std::printf("no NDI source matching '%s' found within %u ms\n", argv[2], timeout);
            return 3;
        }

        std::printf("receiving metadata from '%s'...\n", source->p_ndi_name);
        const uint32_t received = receiveMetadata(*source, timeout, UINT32_MAX);
        return received > 0 ? 0 : 3;
    }

    if (mode == "send")
    {
        const std::string name = argc > 2 ? argv[2] : "LingoFlow";
        const uint32_t seconds = static_cast<uint32_t>(parseNumber(argc > 3 ? argv[3] : nullptr, 10));

        NDIlib_send_instance_t sender = createSender(name);
        const uint32_t sent = sendCaptions(sender, seconds);
        requireApi().send_destroy(sender);

        std::printf("send: %u captions to '%s'\n", sent, name.c_str());
        return 0;
    }

    if (mode == "selfcheck")
        return selfcheck(static_cast<uint32_t>(parseNumber(argc > 2 ? argv[2] : nullptr, 10000)));

    std::printf("unknown mode '%s'\n", mode.c_str());
    return 2;
}
