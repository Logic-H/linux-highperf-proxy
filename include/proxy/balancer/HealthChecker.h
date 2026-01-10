#pragma once

#include "proxy/network/EventLoop.h"
#include "proxy/network/InetAddress.h"
#include <functional>
#include <memory>

namespace proxy {
namespace balancer {

class HealthChecker {
public:
    using CheckCallback = std::function<void(bool healthy, const proxy::network::InetAddress& addr)>;

    explicit HealthChecker(proxy::network::EventLoop* loop) : loop_(loop) {}
    virtual ~HealthChecker() = default;

    // Async check. Callback will be called on loop thread.
    virtual void Check(const proxy::network::InetAddress& addr, CheckCallback cb) = 0;

protected:
    proxy::network::EventLoop* loop_;
};

using HealthCheckerPtr = std::shared_ptr<HealthChecker>;

} // namespace balancer
} // namespace proxy
