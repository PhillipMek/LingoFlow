#pragma once
//
// FakeWebSocketTransport - the scripted transport that lets the OpenAI backend
// of task 009 be tested without a network (same role MockTranslationBackend
// plays for the contract: it lives in the test tree only).
//
// Model: a queue of scripted ReceiveEvents consumed in order by receive(); an
// empty queue returns a quick timeout (with a small sleep, so the backend loop
// does not hot-spin the test binary). Every sendText() frame is recorded for
// inspection. The transport itself is mutex-protected: the backend hands
// frames from its loop thread while the test thread observes.
//
// connect() is fully scripted through connectOutcome; nothing here opens a
// socket.

#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Network/IWebSocketTransport.h"

namespace liveai {
namespace test {

class FakeWebSocketTransport final : public network::IWebSocketTransport
{
public:
    network::ConnectResult connectOutcome; // set before openSession()

    void queueMessage(const std::string& json)
    {
        network::ReceiveEvent ev;
        ev.kind = network::ReceiveKind::message;
        ev.text = json;
        queueEvent(ev);
    }

    void queueEvent(const network::ReceiveEvent& ev)
    {
        const std::lock_guard<std::mutex> lock (mutex_);
        pending_.push_back(ev);
    }

    network::ConnectResult connect(const std::string& host,
                                   int port,
                                   const std::string& pathAndQuery,
                                   const std::vector<std::string>& extraHeaders) override
    {
        const std::lock_guard<std::mutex> lock (mutex_);
        ++connects_;
        lastUrl_ = host + pathAndQuery;
        lastPort_ = port;
        lastHeaders_ = extraHeaders;
        return connectOutcome;
    }

    bool sendText(const std::string& utf8, std::string& error) override
    {
        (void)error;
        const std::lock_guard<std::mutex> lock (mutex_);
        if (failSends_)
            return false;
        sent_.push_back(utf8);
        return true;
    }

    void setFailSends(bool value)
    {
        const std::lock_guard<std::mutex> lock (mutex_);
        failSends_ = value;
    }

    /// Gating lets a test pile up drain events that the backend loop must only
    /// see at a chosen moment (the graceful-close test): a gated receive()
    /// answers timeout WITHOUT popping the queue.
    void gate(bool value) { gated_.store(value); }

    network::ReceiveEvent receive() override
    {
        if (cancelled_.load())
        {
            network::ReceiveEvent ev;
            ev.kind = network::ReceiveKind::error;
            ev.transportError = "transport cancelled (mirrors WinHttpWebSocketClose)";
            return ev;
        }
        if (!gated_.load())
        {
            const std::lock_guard<std::mutex> lock (mutex_);
            if (!pending_.empty())
            {
                network::ReceiveEvent ev = pending_.front();
                pending_.pop_front();
                return ev;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds (2));
        return network::ReceiveEvent {}; // kind == timeout
    }

    void closeTransport(const std::string& reason) noexcept override
    {
        {
            const std::lock_guard<std::mutex> lock (mutex_);
            ++closes_;
            lastCloseReason_ = reason;
        }
        // Mirrors the production semantics proven live: closing the socket
        // releases a pending receive with an error (OPERATION_CANCELLED).
        cancelled_.store(true);
    }

    // ------------------------------------------------------------- observations
    std::vector<std::string> sent() const
    {
        const std::lock_guard<std::mutex> lock (mutex_);
        return sent_;
    }

    std::string lastUrl() const
    {
        const std::lock_guard<std::mutex> lock (mutex_);
        return lastUrl_;
    }

    int lastPort() const
    {
        const std::lock_guard<std::mutex> lock (mutex_);
        return lastPort_;
    }

    std::vector<std::string> lastHeaders() const
    {
        const std::lock_guard<std::mutex> lock (mutex_);
        return lastHeaders_;
    }

    int connects() const
    {
        const std::lock_guard<std::mutex> lock (mutex_);
        return connects_;
    }

    int closes() const
    {
        const std::lock_guard<std::mutex> lock (mutex_);
        return closes_;
    }

private:
    mutable std::mutex mutex_;
    std::deque<network::ReceiveEvent> pending_;
    std::vector<std::string> sent_;
    std::string lastUrl_;
    std::string lastCloseReason_;
    std::vector<std::string> lastHeaders_;
    int lastPort_ = 0;
    int connects_ = 0;
    int closes_ = 0;
    bool failSends_ = false;
    std::atomic<bool> gated_ { false };
    std::atomic<bool> cancelled_ { false };
};

} // namespace test
} // namespace liveai
