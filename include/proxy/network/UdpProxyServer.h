#pragma once

#include "proxy/common/noncopyable.h"
#include "proxy/network/Channel.h"
#include "proxy/network/EventLoop.h"
#include "proxy/network/InetAddress.h"
#include "proxy/balancer/BackendManager.h"

#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>

namespace proxy {
namespace network {

// A minimal UDP proxy:
// - Receives datagrams from clients on listen_port
// - Selects a backend via BackendManager
// - For each client, creates a connected UDP socket to backend and forwards data
// - Forwards backend replies back to the client via the listening socket
// - Cleans idle client sessions periodically (timerfd)
class UdpProxyServer : proxy::common::noncopyable {
public:
    UdpProxyServer(EventLoop* loop, uint16_t listenPort, const std::string& name = "UdpProxy");
    ~UdpProxyServer();

    void Start();
    void StartHealthCheck(double intervalSec = 5.0) { backendManager_.StartHealthCheck(intervalSec); }

    void AddBackend(const std::string& ip, uint16_t port, int weight = 1) { backendManager_.AddBackend(ip, port, weight); }
    void SetIdleTimeout(double idleTimeoutSec, double cleanupIntervalSec = 1.0);

private:
    struct Session {
        int fd{-1}; // connected UDP socket to backend
        sockaddr_in clientAddr{};
        InetAddress backendAddr{0};
        std::unique_ptr<Channel> channel;
        std::chrono::steady_clock::time_point lastActive{};
    };

    void OnClientReadable(std::chrono::system_clock::time_point);
    void OnBackendReadable(int sessionFd, std::chrono::system_clock::time_point);

    std::string ClientKey(const sockaddr_in& addr) const;
    Session* GetOrCreateSession(const sockaddr_in& clientAddr);
    void CloseSession(const std::string& key);

    void StartCleanupTimer();
    void StopCleanupTimer();
    void OnCleanupTimerReadable(std::chrono::system_clock::time_point);
    void CleanupIdleSessions();

    EventLoop* loop_;
    const std::string name_;
    int listenFd_{-1};
    std::unique_ptr<Channel> listenChannel_;

    balancer::BackendManager backendManager_;

    double idleTimeoutSec_{10.0};
    double cleanupIntervalSec_{1.0};
    int cleanupTimerFd_{-1};
    std::unique_ptr<Channel> cleanupTimerChannel_;

    std::unordered_map<std::string, Session> sessions_;
    std::unordered_map<int, std::string> fdToClientKey_;
};

} // namespace network
} // namespace proxy
