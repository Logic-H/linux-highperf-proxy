#include "proxy/balancer/BackendConnectionPool.h"
#include "proxy/common/Logger.h"

#include <utility>

namespace proxy {
namespace balancer {

BackendConnectionPool::Lease::Lease(proxy::network::EventLoop* loop,
                                   std::string backendId,
                                   std::shared_ptr<proxy::network::TcpClient> client,
                                   BackendConnectionPool* pool)
    : loop_(loop), backendId_(std::move(backendId)), client_(std::move(client)), pool_(pool) {
}

proxy::network::TcpConnectionPtr BackendConnectionPool::Lease::connection() const {
    if (!client_) return {};
    return client_->connection();
}

void BackendConnectionPool::Lease::Release(bool keepAlive) {
    if (released_) return;
    released_ = true;
    if (!pool_) return;
    pool_->ReleaseInternal(loop_, backendId_, client_, keepAlive);
    client_.reset();
}

BackendConnectionPool::BackendConnectionPool() : cfg_() {}

BackendConnectionPool::BackendConnectionPool(Config cfg) : cfg_(cfg) {}

void BackendConnectionPool::Acquire(proxy::network::EventLoop* loop,
                                    const proxy::network::InetAddress& backend,
                                    AcquireCallback cb) {
    if (!loop) {
        if (cb) cb(nullptr);
        return;
    }
    const std::string id = backend.toIpPort();

    // Fast path: reuse idle connection in this loop.
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto& perLoop = pools_[loop];
        auto& pb = perLoop.backends[id];
        while (!pb.idle.empty()) {
            auto client = pb.idle.back();
            pb.idle.pop_back();
            if (!client) continue;
            auto conn = client->connection();
            if (conn && conn->connected()) {
                auto lease = std::make_shared<Lease>(loop, id, client, this);
                lease->SetBackendAddr(backend);
                if (cb) cb(std::move(lease));
                return;
            }
        }
    }

    // Create a new client and connect.
    auto client = std::make_shared<proxy::network::TcpClient>(loop, backend, "PoolBackend");
    auto lease = std::make_shared<Lease>(loop, id, client, this);
    lease->SetBackendAddr(backend);

    auto done = std::make_shared<bool>(false);

    client->SetConnectionCallback([cb, done, lease](const proxy::network::TcpConnectionPtr& c) {
        if (*done) return;
        if (c && c->connected()) {
            *done = true;
            if (cb) cb(lease);
        } else {
            // connect failed / disconnected before use
            *done = true;
            if (cb) cb(nullptr);
        }
    });
    client->Connect();
}

void BackendConnectionPool::ReleaseInternal(proxy::network::EventLoop* loop,
                                            const std::string& backendId,
                                            std::shared_ptr<proxy::network::TcpClient> client,
                                            bool keepAlive) {
    if (!keepAlive || !loop || !client) {
        if (client) client->Disconnect();
        return;
    }
    auto conn = client->connection();
    if (!conn || !conn->connected()) {
        client->Disconnect();
        return;
    }

    std::lock_guard<std::mutex> lock(mu_);
    auto& perLoop = pools_[loop];
    auto& pb = perLoop.backends[backendId];
    if (pb.idle.size() >= cfg_.maxIdlePerBackend) {
        client->Disconnect();
        return;
    }
    pb.idle.push_back(std::move(client));
}

} // namespace balancer
} // namespace proxy
