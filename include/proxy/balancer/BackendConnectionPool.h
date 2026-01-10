#pragma once

#include "proxy/common/noncopyable.h"
#include "proxy/network/EventLoop.h"
#include "proxy/network/InetAddress.h"
#include "proxy/network/TcpClient.h"

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace proxy {
namespace balancer {

// Simple per-backend keep-alive connection pool (one in-flight request per connection).
// Designed for HTTP/1.1 proxying to reduce connect/handshake overhead.
class BackendConnectionPool : proxy::common::noncopyable {
public:
    struct Config {
        size_t maxIdlePerBackend{32};
    };

    class Lease : proxy::common::noncopyable {
    public:
        Lease(proxy::network::EventLoop* loop,
              std::string backendId,
              std::shared_ptr<proxy::network::TcpClient> client,
              BackendConnectionPool* pool);

        proxy::network::TcpConnectionPtr connection() const;
        const std::string& backendId() const { return backendId_; }
        const proxy::network::InetAddress& backendAddr() const { return backendAddr_; }

        void SetBackendAddr(const proxy::network::InetAddress& addr) { backendAddr_ = addr; }

        // keepAlive=true -> return to pool; else drop.
        void Release(bool keepAlive);

    private:
        proxy::network::EventLoop* loop_;
        std::string backendId_;
        proxy::network::InetAddress backendAddr_{0};
        std::shared_ptr<proxy::network::TcpClient> client_;
        BackendConnectionPool* pool_{nullptr};
        bool released_{false};
    };

    using AcquireCallback = std::function<void(std::shared_ptr<Lease> lease)>;

    BackendConnectionPool();
    explicit BackendConnectionPool(Config cfg);
    ~BackendConnectionPool() = default;

    void Acquire(proxy::network::EventLoop* loop, const proxy::network::InetAddress& backend, AcquireCallback cb);

private:
    void ReleaseInternal(proxy::network::EventLoop* loop,
                         const std::string& backendId,
                         std::shared_ptr<proxy::network::TcpClient> client,
                         bool keepAlive);

    struct PerBackend {
        std::vector<std::shared_ptr<proxy::network::TcpClient>> idle;
    };

    struct PerLoop {
        std::unordered_map<std::string, PerBackend> backends;
    };

    Config cfg_;
    std::mutex mu_;
    std::unordered_map<proxy::network::EventLoop*, PerLoop> pools_;
};

} // namespace balancer
} // namespace proxy
