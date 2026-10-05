#include "Network/WinHttpTransport.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace liveai {
namespace network {

namespace {

std::string lastWinHttpError(DWORD code)
{
    char message[160];
    std::snprintf(message, sizeof(message), "winhttp error %lu", static_cast<unsigned long>(code));
    return message;
}

std::wstring toWide(const std::string& utf8)
{
    if (utf8.empty())
        return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                          static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), count);
    return out;
}

std::string toNarrow(const wchar_t* wide, std::size_t chars)
{
    if (wide == nullptr || chars == 0)
        return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(chars),
                                          nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(chars), out.data(), count,
                        nullptr, nullptr);
    return out;
}

/// Cap for the captured refusal body: the JSON error object is tiny, and this
/// is a defensive ceiling against a chatty peer, not a format limit.
constexpr DWORD kMaxRefusalBodyBytes = 4096;

/// A refused upgrade may carry guidance (protocol doc section 9): the
/// `Retry-After` header - integer-seconds form only; the HTTP-date form is not
/// used by this service and is treated as "no hint" - and a small JSON body
/// with the error code. Capturing is best-effort: nothing here can break the
/// refusal itself, and the caller keeps its `transportError` verdict.
void captureRefusal(HINTERNET request, ConnectResult& result)
{
    wchar_t header[64] = {};
    DWORD size = sizeof(header);
    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, L"Retry-After", header, &size,
                            WINHTTP_NO_HEADER_INDEX))
    {
        const std::string value = toNarrow(header, size / sizeof(wchar_t));
        try
        {
            const long seconds = std::stol(value);
            if (seconds > 0 && seconds <= 86400L)
                result.retryAfterSec = static_cast<int>(seconds);
        }
        catch (const std::exception&)
        {
            // not an integer-seconds Retry-After: no hint
        }
    }

    DWORD available = 0;
    if (WinHttpQueryDataAvailable(request, &available))
    {
        char chunk[512];
        while (available > 0 && result.refusalBody.size() < kMaxRefusalBodyBytes)
        {
            const DWORD room = kMaxRefusalBodyBytes - static_cast<DWORD>(result.refusalBody.size());
            const DWORD want = (std::min)({ available, static_cast<DWORD>(sizeof(chunk)), room });
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk, want, &read) || read == 0)
                break;
            result.refusalBody.append(chunk, read);
            if (!WinHttpQueryDataAvailable(request, &available))
                break;
        }
    }
}

} // namespace

WinHttpTransport::WinHttpTransport()
{
    recvBuffer_.resize(64 * 1024);
}

WinHttpTransport::~WinHttpTransport()
{
    closeTransport("released");
}

ConnectResult WinHttpTransport::connect(const std::string& host,
                                        int port,
                                        const std::string& pathAndQuery,
                                        const std::vector<std::string>& extraHeaders)
{
    ConnectResult result;
    releaseHandles();
    peerClosed_.store(false);
    partialMessage_.clear();
    inboundBudget_.reset();   // a new connection reassembles from zero

    const HINTERNET session = WinHttpOpen(L"LingoFlow/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr)
    {
        result.transportError = lastWinHttpError(GetLastError());
        return result;
    }
    session_.store(session);

    WinHttpSetTimeouts(session, 5000 /*resolve*/, 10000 /*connect*/, 10000 /*send*/, 5000 /*receive*/);

    // TLS 1.2 minimum (1.3 included when the OS provides it) - AGENTS.md 21
    // requires modern transport for the credential-bearing connection.
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));

    const std::wstring wideHost = toWide(host);
    const std::wstring widePath = toWide(pathAndQuery);

    const HINTERNET connection = WinHttpConnect(session, wideHost.c_str(),
                                                static_cast<INTERNET_PORT>(port), 0);
    if (connection == nullptr)
    {
        result.transportError = lastWinHttpError(GetLastError());
        return result;
    }
    connection_.store(connection);

    const HINTERNET request = WinHttpOpenRequest(connection, L"GET", widePath.c_str(), nullptr,
                                                 WINHTTP_NO_REFERER,
                                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                 WINHTTP_FLAG_SECURE);
    if (request == nullptr)
    {
        result.transportError = lastWinHttpError(GetLastError());
        return result;
    }
    request_.store(request);

    for (const std::string& header : extraHeaders)
    {
        const std::wstring wideHeader = toWide(header);
        if (!WinHttpAddRequestHeaders(request, wideHeader.c_str(),
                                      static_cast<DWORD>(wideHeader.size()),
                                      WINHTTP_ADDREQ_FLAG_ADD))
        {
            result.transportError = lastWinHttpError(GetLastError());
            return result;
        }
    }

    if (!WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0))
    {
        result.transportError = lastWinHttpError(GetLastError());
        return result;
    }

    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        || !WinHttpReceiveResponse(request, nullptr))
    {
        const DWORD code = GetLastError();
        DWORD status = 0;
        DWORD size = sizeof(status);
        if (WinHttpQueryHeaders(request,
                               WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                               WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                               WINHTTP_NO_HEADER_INDEX))
            result.httpStatus = static_cast<int>(status);
        result.transportError = lastWinHttpError(code);
        return result;
    }

    DWORD status = 0;
    DWORD size = sizeof(status);
    if (WinHttpQueryHeaders(request,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                            WINHTTP_NO_HEADER_INDEX))
        result.httpStatus = static_cast<int>(status);

    if (status != 101)
    {
        captureRefusal(request, result); // protocol section 9 guidance
        return result; // refused upgrade; the backend maps the HTTP status
    }

    const HINTERNET webSocket = WinHttpWebSocketCompleteUpgrade(request, 0);
    if (webSocket == nullptr)
    {
        result.transportError = lastWinHttpError(GetLastError());
        return result;
    }
    // On success WinHttpWebSocketCompleteUpgrade consumes the request handle.
    request_.store(nullptr);
    webSocket_.store(webSocket);

    // No receive timeout is configured: live diagnosis proved a pending
    // synchronous WinHttpWebSocketReceive is not released by receive timeouts,
    // and the backend therefore runs the receive on its own thread and bounds
    // waiting by closing the handle from the other side (the cancellation path
    // validated in the same diagnostic). The HTTP-phase timeouts above are
    // what bound connect().

    // The live probe (protocol doc section 15) saw the service close silent
    // connections with "keepalive ping timeout". WinHTTP's WebSocket keepalive
    // emits periodic client-side pings at the transport level - exactly the
    // requirement - and WinHTTP answers server pings automatically. The
    // documented minimum interval is 15 s.
    DWORD keepalive = WINHTTP_WEB_SOCKET_MIN_KEEPALIVE_VALUE;
    WinHttpSetOption(webSocket, WINHTTP_OPTION_WEB_SOCKET_KEEPALIVE_INTERVAL, &keepalive,
                     sizeof(keepalive));

    DWORD closeTimeout = 2000;
    WinHttpSetOption(webSocket, WINHTTP_OPTION_WEB_SOCKET_CLOSE_TIMEOUT, &closeTimeout,
                     sizeof(closeTimeout));

    result.upgraded = true;
    result.httpStatus = 101;
    return result;
}

bool WinHttpTransport::sendText(const std::string& utf8, std::string& error)
{
    const HINTERNET ws = static_cast<HINTERNET>(webSocket_.load());
    if (ws == nullptr || peerClosed_.load())
    {
        error = "transport is not connected";
        return false;
    }

    const DWORD res = WinHttpWebSocketSend(ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                           const_cast<char*>(utf8.data()),
                                           static_cast<DWORD>(utf8.size()));
    if (res != ERROR_SUCCESS)
    {
        error = lastWinHttpError(res);
        return false;
    }
    return true;
}

ReceiveEvent WinHttpTransport::receive()
{
    ReceiveEvent event;
    const HINTERNET ws = static_cast<HINTERNET>(webSocket_.load());
    if (ws == nullptr || peerClosed_.load())
    {
        event.kind = ReceiveKind::closed;
        return event;
    }

    for (;;)
    {
        DWORD transferred = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE bufferType {};
        const DWORD res = WinHttpWebSocketReceive(ws, recvBuffer_.data(),
                                                  static_cast<DWORD>(recvBuffer_.size()),
                                                  &transferred, &bufferType);

        if (res != ERROR_SUCCESS)
        {
            if (res == ERROR_WINHTTP_TIMEOUT)
            {
                event.kind = partialMessage_.empty() ? ReceiveKind::timeout : ReceiveKind::error;
                if (!partialMessage_.empty())
                    event.transportError = "receive window elapsed inside a fragmented message";
            }
            else
                event.kind = ReceiveKind::error;
            if (event.kind == ReceiveKind::error)
                event.transportError = lastWinHttpError(res);
            return event;
        }

        // One reassembly budget covers the whole message (all fragments plus
        // the final buffer). Accepting each chunk before appending bounds the
        // accumulated size; on refusal the connection is failed once and the
        // supervisor recovers - the stream cannot grow the process (code
        // review P1, Network/NetworkLimits.h).
        const auto admit = [&](DWORD bytes) -> bool {
            if (inboundBudget_.accept(static_cast<std::size_t>(bytes)))
                return true;
            partialMessage_.clear();
            inboundBudget_.reset();
            event.kind = ReceiveKind::error;
            event.transportError = "inbound WebSocket message exceeded the reassembly budget";
            return false;
        };

        switch (bufferType)
        {
            case WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE:
            {
                if (!admit(transferred))
                    return event;
                partialMessage_.append(recvBuffer_.data(), transferred);
                event.kind = ReceiveKind::message;
                event.text = std::move(partialMessage_);
                partialMessage_.clear();
                inboundBudget_.reset();   // message complete: the next starts fresh
                return event;
            }
            case WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE:
            case WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE:
                if (!admit(transferred))
                    return event;
                partialMessage_.append(recvBuffer_.data(), transferred);
                continue; // stay inside this receive() call until the message completes
            case WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE:
                // Binary payloads are not part of this protocol (all events are
                // JSON text, docs/openai-realtime-protocol.md section 6). Hand
                // the bytes to the parser and let it report the shape break.
                if (!admit(transferred))
                    return event;
                partialMessage_.append(recvBuffer_.data(), transferred);
                event.kind = ReceiveKind::message;
                event.text = std::move(partialMessage_);
                partialMessage_.clear();
                inboundBudget_.reset();   // message complete: the next starts fresh
                return event;
            case WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE:
            {
                peerClosed_.store(true);
                event.kind = ReceiveKind::closed;
                if (transferred >= 2)
                {
                    const auto* bytes = reinterpret_cast<const unsigned char*>(recvBuffer_.data());
                    event.closeCode = (bytes[0] << 8) | bytes[1];
                    event.closeReason.assign(reinterpret_cast<const char*>(bytes + 2),
                                             transferred - 2);
                }
                return event;
            }
        }
    }
}

void WinHttpTransport::closeTransport(const std::string& reason) noexcept
{
    // May be called concurrently (receiver's exit path and a forced drain
    // cancel from closeSession): exchange steals each handle exactly once, so
    // no double close and no use-after-reset. WinHttpWebSocketClose is also
    // the documented mechanism that releases a pending WinHttpWebSocketReceive
    // with ERROR_WINHTTP_OPERATION_CANCELLED (validated live).
    peerClosed_.store(true);

    const HINTERNET ws = static_cast<HINTERNET>(webSocket_.exchange(nullptr));
    if (ws != nullptr)
    {
        // Close initiates the RFC 6455 close handshake (best effort; a peer
        // that already hung up just fails the call, which is fine); Shutdown
        // ends the local socket in both directions. The reason is capped to
        // the documented maximum.
        const std::string capped = reason.substr(0, WINHTTP_WEB_SOCKET_MAX_CLOSE_REASON_LENGTH);
        WinHttpWebSocketClose(ws, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS,
                              capped.empty() ? nullptr : const_cast<char*>(capped.data()),
                              static_cast<DWORD>(capped.size()));
        WinHttpWebSocketShutdown(ws, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
        WinHttpCloseHandle(ws);
    }

    releaseHandles(); // releases the remaining handle chain (ws already freed)
}

void WinHttpTransport::releaseHandles() noexcept
{
    void* ws = webSocket_.exchange(nullptr);
    if (ws != nullptr)
        WinHttpCloseHandle(static_cast<HINTERNET>(ws));

    void* request = request_.exchange(nullptr);
    if (request != nullptr)
        WinHttpCloseHandle(static_cast<HINTERNET>(request));

    void* connection = connection_.exchange(nullptr);
    if (connection != nullptr)
        WinHttpCloseHandle(static_cast<HINTERNET>(connection));

    void* session = session_.exchange(nullptr);
    if (session != nullptr)
        WinHttpCloseHandle(static_cast<HINTERNET>(session));
}

std::unique_ptr<IWebSocketTransport> makeWinHttpTransport()
{
    return std::make_unique<WinHttpTransport>();
}

} // namespace network
} // namespace liveai
