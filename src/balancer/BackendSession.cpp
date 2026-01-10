#include "proxy/balancer/BackendSession.h"
#include "proxy/balancer/BackendManager.h"
#include "proxy/network/EventLoop.h"
#include "proxy/common/Logger.h"

namespace proxy {
namespace balancer {

BackendSession::BackendSession(proxy::network::EventLoop* loop,
                               const std::string& backendIp,
                               uint16_t backendPort,
                               const proxy::network::TcpConnectionPtr& clientConn,
                               proxy::balancer::BackendManager* backendManager,
                               TunnelConfig tunnelCfg)
    : loop_(loop),
      backendClient_(loop, proxy::network::InetAddress(backendIp, backendPort), "Backend"),
      clientConn_(clientConn),
      backendConn_(),
      connected_(false),
      backendManager_(backendManager),
      backendAddr_(backendIp, backendPort),
      firstResponseRecorded_(false),
      startTime_(std::chrono::steady_clock::now()),
      tunnelCfg_(tunnelCfg) {
    
    backendClient_.SetConnectionCallback(
        std::bind(&BackendSession::OnBackendConnection, this, std::placeholders::_1));
    backendClient_.SetMessageCallback(
        std::bind(&BackendSession::OnBackendMessage, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
}

BackendSession::~BackendSession() {
    LOG_INFO << "BackendSession destroyed";
}

void BackendSession::Start() {
    startTime_ = std::chrono::steady_clock::now();
    firstResponseRecorded_ = false;
    if (tunnelCfg_.pauseClientReadUntilBackendConnected) {
        if (auto client = clientConn_.lock()) {
            client->StopRead();
        }
    }
    backendClient_.Connect();
}

void BackendSession::Send(const std::string& msg) {
    if (connected_) {
        auto conn = backendClient_.connection();
        if (conn) {
            conn->Send(msg);
        }
    } else {
        initialBuffer_ += msg;
    }
}

void BackendSession::Send(const void* data, size_t len) {
    if (connected_) {
        auto conn = backendClient_.connection();
        if (conn) {
            conn->Send(data, len);
        }
    } else {
        initialBuffer_.append(static_cast<const char*>(data), len);
    }
}

void BackendSession::OnBackendConnection(const proxy::network::TcpConnectionPtr& conn) {
    if (conn->connected()) {
        LOG_INFO << "Connected to backend " << conn->peerAddress().toIpPort();
        connected_ = true;
        backendConn_ = conn;

        const auto client = clientConn_.lock();
        if (client && tunnelCfg_.enableBackpressure) {
            const size_t hwm = tunnelCfg_.highWaterMarkBytes;
            std::weak_ptr<proxy::network::TcpConnection> wClient = client;
            std::weak_ptr<proxy::network::TcpConnection> wBackend = backendConn_;

            // Client -> Backend: when backend output buffer is too large, stop reading from client.
            backendConn_->SetHighWaterMarkCallback(
                [wClient](const proxy::network::TcpConnectionPtr&, size_t) {
                    if (auto c = wClient.lock()) c->StopRead();
                },
                hwm);
            backendConn_->SetWriteCompleteCallback(
                [wClient](const proxy::network::TcpConnectionPtr&) {
                    if (auto c = wClient.lock()) c->StartRead();
                });

            // Backend -> Client: when client output buffer is too large, stop reading from backend.
            client->SetHighWaterMarkCallback(
                [wBackend](const proxy::network::TcpConnectionPtr&, size_t) {
                    if (auto b = wBackend.lock()) b->StopRead();
                },
                hwm);
            client->SetWriteCompleteCallback(
                [wBackend](const proxy::network::TcpConnectionPtr&) {
                    if (auto b = wBackend.lock()) b->StartRead();
                });
        }

        if (backendManager_) {
            backendManager_->OnBackendConnectionStart(backendAddr_);
        }
        if (!initialBuffer_.empty()) {
            conn->Send(initialBuffer_);
            initialBuffer_.clear();
        }
        if (client && tunnelCfg_.pauseClientReadUntilBackendConnected) {
            client->StartRead();
        }
    } else {
        LOG_INFO << "Disconnected from backend";
        connected_ = false;
        backendConn_.reset();
        if (backendManager_) {
            backendManager_->OnBackendConnectionEnd(backendAddr_);
            // Passive failover: if backend disconnects before any response is observed,
            // treat it as a failure signal and temporarily mark backend DOWN.
            if (!firstResponseRecorded_) {
                backendManager_->ReportBackendFailure(backendAddr_);
            }
        }
        // Close frontend if backend closes?
        auto client = clientConn_.lock();
        if (client) {
            client->Shutdown();
        }
    }
}

void BackendSession::OnBackendMessage(const proxy::network::TcpConnectionPtr& conn,
                                      proxy::network::Buffer* buf,
                                      std::chrono::system_clock::time_point) {
    if (!firstResponseRecorded_) {
        firstResponseRecorded_ = true;
        if (backendManager_) {
            const auto now = std::chrono::steady_clock::now();
            const auto ms = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(now - startTime_).count();
            backendManager_->RecordBackendResponseTimeMs(backendAddr_, ms);
        }
    }
    (void)conn;
    auto client = clientConn_.lock();
    if (client) {
        if (buf->ReadableBytes() > 0) {
            client->Send(buf->Peek(), buf->ReadableBytes());
        }
        buf->RetrieveAll();
    } else {
        buf->RetrieveAll();
    }
}

} // namespace balancer
} // namespace proxy
