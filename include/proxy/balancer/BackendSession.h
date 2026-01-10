#pragma once

#include "proxy/common/noncopyable.h"
#include "proxy/network/TcpClient.h"
#include "proxy/network/TcpConnection.h"
#include "proxy/protocol/HttpRequest.h"

#include <memory>
#include <cstddef>

namespace proxy {
namespace balancer {

class BackendManager;

class BackendSession : public std::enable_shared_from_this<BackendSession>,
                       proxy::common::noncopyable {
public:
    struct TunnelConfig {
        bool enableBackpressure;
        bool pauseClientReadUntilBackendConnected;
        size_t highWaterMarkBytes;

        TunnelConfig()
            : enableBackpressure(false),
              pauseClientReadUntilBackendConnected(false),
              highWaterMarkBytes(8 * 1024 * 1024) {}
    };

    BackendSession(proxy::network::EventLoop* loop,
                   const std::string& backendIp,
                   uint16_t backendPort,
                   const proxy::network::TcpConnectionPtr& clientConn,
                   proxy::balancer::BackendManager* backendManager = nullptr,
                   TunnelConfig tunnelCfg = TunnelConfig());
    
    ~BackendSession();

    void Start();
    void Send(const std::string& msg);
    void Send(const void* data, size_t len);

private:
    void OnBackendConnection(const proxy::network::TcpConnectionPtr& conn);
    void OnBackendMessage(const proxy::network::TcpConnectionPtr& conn,
                          proxy::network::Buffer* buf,
                          std::chrono::system_clock::time_point);
    void OnBackendClose(const proxy::network::TcpConnectionPtr& conn);

    proxy::network::EventLoop* loop_;
    proxy::network::TcpClient backendClient_;
    std::weak_ptr<proxy::network::TcpConnection> clientConn_; // Frontend connection
    proxy::network::TcpConnectionPtr backendConn_;
    bool connected_;
    std::string initialBuffer_; // Buffer data arrived before connection established

    proxy::balancer::BackendManager* backendManager_{nullptr};
    proxy::network::InetAddress backendAddr_{0};
    bool firstResponseRecorded_{false};
    std::chrono::steady_clock::time_point startTime_{};
    TunnelConfig tunnelCfg_{};
};

using BackendSessionPtr = std::shared_ptr<BackendSession>;

} // namespace balancer
} // namespace proxy
