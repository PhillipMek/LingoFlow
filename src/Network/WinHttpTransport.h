#pragma once
//
// WinHttpTransport - the production IWebSocketTransport on winhttp.dll
// (task 009). Windows ships the WebSocket framing, TLS and HTTP upgrade, so
// this product needs no third-party network dependency (AGENTS.md 15): the
// whole file is a thin, honest wrapper around the documented WinHTTP WebSocket
// calls.
//
// Threading: a WinHTTP websocket handle is used full-duplex - one thread sends
// (append cadence, session.close) while another is blocked in receive, and a
// third may cancel the blocked receive by closing the handle. That is exactly
// the model the backend runs (OpenAIRealtimeBackend.h) and was validated live
// (39 concurrent sends while receive blocked, and a close releasing the
// pending receive after 225 ms). Hence the handles are atomics and only the
// receive thread touches the receive buffer / partial-message state.

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "Network/IWebSocketTransport.h"
#include "Network/NetworkLimits.h"

namespace liveai {
namespace network {

/// Live diagnosis (protocol doc section 15) proved a pending synchronous
/// WinHttpWebSocketReceive is not released by receive timeouts. The transport
/// therefore never relies on receive timeouts for pacing: the backend runs the
/// receive on a dedicated thread (see OpenAIRealtimeBackend.h) and bounds any
/// wait by closing the handle, which does release a blocked receive.
class WinHttpTransport final : public IWebSocketTransport
{
public:
    WinHttpTransport();
    ~WinHttpTransport() override;

    WinHttpTransport(const WinHttpTransport&) = delete;
    WinHttpTransport& operator=(const WinHttpTransport&) = delete;

    ConnectResult connect(const std::string& host,
                          int port,
                          const std::string& pathAndQuery,
                          const std::vector<std::string>& extraHeaders) override;

    bool sendText(const std::string& utf8, std::string& error) override;
    ReceiveEvent receive() override;
    void closeTransport(const std::string& reason) noexcept override;

private:
    void releaseHandles() noexcept;

    // HINTERNET is HANDLE == void* on the Windows SDK; kept opaque here so no
    // consumer of this header ever touches <windows.h>. Atomics because send,
    // receive and close can run on different threads against one handle.
    std::atomic<void*> session_ { nullptr };
    std::atomic<void*> connection_ { nullptr };
    std::atomic<void*> request_ { nullptr };
    std::atomic<void*> webSocket_ { nullptr };

    // Receive-thread-only state (never touched by sendText).
    std::vector<char> recvBuffer_;
    std::string partialMessage_;
    InboundMessageBudget inboundBudget_;   ///< bounds reassembly (code review P1)
    std::atomic<bool> peerClosed_ { false };
};

} // namespace network
} // namespace liveai
