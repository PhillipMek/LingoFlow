#pragma once
//
// IWebSocketTransport - the tiny seam between the OpenAI backend and whatever
// actually moves WebSocket frames.
//
// Why a seam at all: the backend's job is the protocol and the contract mapping;
// the OS transport is not testable offline. Everything here is synchronous and
// blocking-by-contract: this interface is only ever used from worker/network
// threads, never the audio callback (the project rules, docs/threading.md). Live
// behavior (protocol doc section 15) fixed the semantics: receive() blocks
// until a frame arrives - it may block INDEFINITELY on an idle socket and is
// not released by timeouts - so the backend dedicates one thread to receiving
// and another to the send cadence (full duplex, validated against the real
// service), and releases a blocked receive by calling closeTransport() from
// the other side.
//
// What the production implementation (WinHttpTransport, winhttp.dll) does with
// protocol control frames: WinHTTP answers server pings automatically, emits
// client-side pings on its documented WebSocket keepalive interval, and only
// surfaces message/close payloads here. On top of that, the backend's
// continuous 200 ms append cadence (protocol doc section 7, "keep appending,
// including silence") is the application-level keepalive, and is strictly more
// traffic than a ping would be. Both layers together answer the live finding in
// the protocol doc section 15 (silent connections get closed with "keepalive
// ping timeout"); the real traffic checkpoint re-checks the combination.

#include <string>
#include <vector>
#include <memory>

namespace liveai {
namespace network {

enum class ReceiveKind
{
    message = 0, ///< a complete text frame in `text`
    timeout,     ///< nothing arrived within a wait window (test transports;
                 ///< the production transport blocks instead of timing out)
    closed,      ///< the peer closed the connection (closeCode/closeReason best-effort)
    error        ///< transport-level failure; transportError describes it
};

struct ReceiveEvent
{
    ReceiveKind kind = ReceiveKind::timeout;
    std::string text;
    int closeCode = 0;
    std::string closeReason;
    std::string transportError;
};

struct ConnectResult
{
    bool upgraded = false; ///< true only when the WS handshake completed (HTTP 101)
    int httpStatus = 0;    ///< HTTP status when the upgrade was refused, else 0
    std::string transportError;

    /// Protocol section 9: a service that refuses the upgrade may answer with
    /// `Retry-After` (integer seconds form; the HTTP-date form is not used here
    /// and maps to 0 = "no hint, use the policy's own backoff"). Transport only
    /// captures it - deciding what to do with it is the backend and the supervisor.
    int retryAfterSec = 0;

    /// Body captured on a refused upgrade, truncated (protocol section 9: 429/503
    /// carry an error code that distinguishes retryable from operator-actionable
    /// rejections). Never logged as such; the backend extracts classification
    /// fields only.
    std::string refusalBody;
};

class IWebSocketTransport
{
public:
    virtual ~IWebSocketTransport() = default;

    /// Performs the client handshake. extraHeaders carries the request headers,
    /// each as "Name: value" (the Authorization value is a credential: it is
    /// never logged by anything that receives it, the project rules).
    virtual ConnectResult connect(const std::string& host,
                                  int port,
                                  const std::string& pathAndQuery,
                                  const std::vector<std::string>& extraHeaders) = 0;

    /// One complete UTF-8 text frame. Returns false and describes the failure.
    virtual bool sendText(const std::string& utf8, std::string& error) = 0;

    /// Waits for one complete frame. MAY BLOCK INDEFINITELY on an idle socket
    /// (see the header note): call only from the dedicated receiver thread.
    /// Fragmented messages are reassembled by the implementation before being
    /// returned as a single message event. closeTransport() from another
    /// thread releases a pending receive with an error event.
    virtual ReceiveEvent receive() = 0;

    /// App-initiated close: sends the close frame (best-effort), releases the
    /// socket, and cancels any receive pending on it. Safe to call more than
    /// once, concurrently, and after `closed`.
    virtual void closeTransport(const std::string& reason) noexcept = 0;
};

/// The production factory (winhttp.dll). Declared portably, defined only on
/// Windows - the rest of the product tree never includes <winhttp.h>.
std::unique_ptr<IWebSocketTransport> makeWinHttpTransport();

} // namespace network
} // namespace liveai
