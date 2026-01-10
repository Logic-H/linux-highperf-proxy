#pragma once

#include "proxy/balancer/HealthChecker.h"
#include "proxy/network/Channel.h"
#include <map>

namespace proxy {
namespace balancer {

// Performs a simple TCP connect check with timeout
class TcpHealthChecker : public HealthChecker {
public:
    TcpHealthChecker(proxy::network::EventLoop* loop, double timeoutSec = 2.0);
    ~TcpHealthChecker() override = default;

    void Check(const proxy::network::InetAddress& addr, CheckCallback cb) override;

private:
    struct CheckContext {
        proxy::network::EventLoop* loop;
        int sockfd;
        int timerfd;
        std::shared_ptr<proxy::network::Channel> connChannel;
        std::shared_ptr<proxy::network::Channel> timerChannel;
        CheckCallback cb;
        proxy::network::InetAddress addr;
        std::atomic<bool> finished{false};
    };

    static void OnConnected(std::shared_ptr<CheckContext> ctx);
    static void OnError(std::shared_ptr<CheckContext> ctx);
    static void OnTimeout(std::shared_ptr<CheckContext> ctx);
    static bool CleanUp(std::shared_ptr<CheckContext> ctx);

    double timeoutSec_;
};

} // namespace balancer
} // namespace proxy
