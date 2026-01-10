#pragma once

#include "proxy/balancer/HealthChecker.h"
#include "proxy/network/Channel.h"

#include <atomic>
#include <memory>
#include <string>

namespace proxy {
namespace balancer {

// Performs a simple HTTP GET check with timeout and validates the status code.
// This is intended for "HTTP status code" health checks in project.txt.
class HttpHealthChecker : public HealthChecker,
                          public std::enable_shared_from_this<HttpHealthChecker> {
public:
    HttpHealthChecker(proxy::network::EventLoop* loop,
                      double timeoutSec = 2.0,
                      std::string hostHeader = "127.0.0.1",
                      std::string path = "/health",
                      int okStatusMin = 200,
                      int okStatusMax = 399);
    ~HttpHealthChecker() override = default;

    void Check(const proxy::network::InetAddress& addr, CheckCallback cb) override;

private:
    enum class State { kConnecting, kSending, kReading };

    struct CheckContext {
        int sockfd{-1};
        int timerfd{-1};
        std::shared_ptr<proxy::network::Channel> connChannel;
        std::shared_ptr<proxy::network::Channel> timerChannel;
        CheckCallback cb;
        proxy::network::InetAddress addr;

        State state{State::kConnecting};
        std::string out;
        size_t outOffset{0};
        std::string in;
        std::atomic<bool> finished{false};
    };

    void OnWritable(std::shared_ptr<CheckContext> ctx);
    void OnReadable(std::shared_ptr<CheckContext> ctx, std::chrono::system_clock::time_point);
    void OnError(std::shared_ptr<CheckContext> ctx);
    void OnTimeout(std::shared_ptr<CheckContext> ctx);
    bool CleanUp(std::shared_ptr<CheckContext> ctx);

    static int ParseHttpStatusCode(const std::string& buf);

    double timeoutSec_;
    std::string hostHeader_;
    std::string path_;
    int okStatusMin_;
    int okStatusMax_;
};

} // namespace balancer
} // namespace proxy

