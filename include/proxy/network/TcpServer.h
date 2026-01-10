#pragma once

#include "proxy/common/noncopyable.h"
#include "proxy/network/InetAddress.h"
#include "proxy/network/Callbacks.h"
#include "proxy/network/TcpConnection.h"
#include "proxy/network/EventLoopThreadPool.h"
#include "proxy/network/TlsContext.h"

#include <map>
#include <string>
#include <atomic>
#include <memory>
#include <unordered_map>

#include "proxy/monitor/TokenBucket.h"
#include "proxy/monitor/PerKeyRateLimiter.h"

namespace proxy {
namespace network {

class EventLoop;
class Acceptor;

class TcpServer : proxy::common::noncopyable {
public:
    enum Option {
        kNoReusePort,
        kReusePort,
    };

    TcpServer(EventLoop* loop,
              const InetAddress& listenAddr,
              const std::string& nameArg,
              Option option = kNoReusePort);
    ~TcpServer();

    const std::string& hostport() const { return hostport_; }
    const std::string& name() const { return name_; }
    EventLoop* getLoop() const { return loop_; }

    void SetThreadNum(int numThreads);

    // TLS termination (optional). If enabled, the listener accepts both HTTPS and plain HTTP by sniffing.
    // - If the first byte looks like TLS handshake (0x16), do TLS handshake and decrypt.
    // - Otherwise, treat as plaintext.
    bool EnableTls(const std::string& certPemPath, const std::string& keyPemPath);

    // Connection limits (0 means unlimited)
    void SetMaxConnections(int maxConnections);
    void SetMaxConnectionsPerIp(int maxConnectionsPerIp);
    // Idle connection cleanup (0 disables). cleanupIntervalSec defaults to 1s if <=0.
    void SetIdleTimeout(double idleTimeoutSec, double cleanupIntervalSec = 1.0);

    // Basic DDoS protection (accept rate limiting). qps<=0 disables.
    void SetAcceptRateLimit(double qps, double burst);
    void SetPerIpAcceptRateLimit(double qps, double burst, double idleSec, size_t maxEntries);

    void Start();

    void SetConnectionCallback(const ConnectionCallback& cb) { connectionCallback_ = cb; }
    void SetMessageCallback(const MessageCallback& cb) { messageCallback_ = cb; }
    void SetWriteCompleteCallback(const WriteCompleteCallback& cb) { writeCompleteCallback_ = cb; }

private:
    void NewConnection(int sockfd, const InetAddress& peerAddr);
    void RemoveConnection(const TcpConnectionPtr& conn);
    void RemoveConnectionInLoop(const TcpConnectionPtr& conn);

    using ConnectionMap = std::map<std::string, TcpConnectionPtr>;

    EventLoop* loop_;
    const std::string hostport_;
    const std::string name_;
    std::unique_ptr<Acceptor> acceptor_;
    
    std::unique_ptr<EventLoopThreadPool> threadPool_;
    std::shared_ptr<proxy::network::TlsContext> tlsCtx_;
    
    ConnectionCallback connectionCallback_;
    MessageCallback messageCallback_;
    WriteCompleteCallback writeCompleteCallback_;

    std::atomic_int started_;
    int next_conn_id_;
    ConnectionMap connections_;

    int maxConnections_{0};
    int maxConnectionsPerIp_{0};
    std::unordered_map<std::string, int> ipConnectionCounts_;

    double idleTimeoutSec_{0.0};
    double cleanupIntervalSec_{1.0};
    int cleanupTimerFd_{-1};
    std::shared_ptr<proxy::network::Channel> cleanupTimerChannel_;

    void StartCleanupTimer();
    void StopCleanupTimer();
    void CleanupIdleConnections();

    std::unique_ptr<proxy::monitor::TokenBucket> acceptRateLimiter_;
    std::unique_ptr<proxy::monitor::PerKeyRateLimiter> perIpAcceptLimiter_;
};

} // namespace network
} // namespace proxy
