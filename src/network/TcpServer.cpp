#include "proxy/network/TcpServer.h"
#include "proxy/network/EventLoop.h"
#include "proxy/network/Acceptor.h"
#include "proxy/network/Channel.h"
#include "proxy/common/Logger.h"
#include "proxy/monitor/Stats.h"

#include <functional>
#include <unistd.h>
#include <sys/timerfd.h>
#include <cerrno>
#include <cstring>

namespace proxy {
namespace network {

TcpServer::TcpServer(EventLoop* loop,
                     const InetAddress& listenAddr,
                     const std::string& nameArg,
                     Option option)
    : loop_(loop),
      hostport_(listenAddr.toIpPort()),
      name_(nameArg),
      acceptor_(new Acceptor(loop, listenAddr, option == kReusePort)),
      threadPool_(new EventLoopThreadPool(loop, nameArg)),
      started_(0),
      next_conn_id_(1) {
    acceptor_->SetNewConnectionCallback(
        std::bind(&TcpServer::NewConnection, this, std::placeholders::_1, std::placeholders::_2));
}

TcpServer::~TcpServer() {
    // ... cleanup ...
    StopCleanupTimer();
}

void TcpServer::SetThreadNum(int numThreads) {
    threadPool_->SetThreadNum(numThreads);
}

bool TcpServer::EnableTls(const std::string& certPemPath, const std::string& keyPemPath) {
    auto ctx = std::make_shared<proxy::network::TlsContext>();
    if (!ctx->InitServer(certPemPath, keyPemPath)) return false;
    tlsCtx_ = std::move(ctx);
    return true;
}

void TcpServer::SetMaxConnections(int maxConnections) {
    maxConnections_ = maxConnections;
}

void TcpServer::SetMaxConnectionsPerIp(int maxConnectionsPerIp) {
    maxConnectionsPerIp_ = maxConnectionsPerIp;
}

void TcpServer::SetIdleTimeout(double idleTimeoutSec, double cleanupIntervalSec) {
    idleTimeoutSec_ = idleTimeoutSec;
    cleanupIntervalSec_ = (cleanupIntervalSec > 0.0) ? cleanupIntervalSec : 1.0;
}

void TcpServer::SetAcceptRateLimit(double qps, double burst) {
    if (qps <= 0.0) {
        acceptRateLimiter_.reset();
        return;
    }
    if (burst <= 0.0) burst = qps;
    acceptRateLimiter_ = std::make_unique<proxy::monitor::TokenBucket>(qps, burst);
}

void TcpServer::SetPerIpAcceptRateLimit(double qps, double burst, double idleSec, size_t maxEntries) {
    if (qps <= 0.0) {
        perIpAcceptLimiter_.reset();
        return;
    }
    proxy::monitor::PerKeyRateLimiter::Config cfg;
    cfg.qps = qps;
    cfg.burst = burst;
    cfg.idleSec = idleSec;
    cfg.maxEntries = maxEntries;
    perIpAcceptLimiter_ = std::make_unique<proxy::monitor::PerKeyRateLimiter>(cfg);
}

void TcpServer::Start() {
    if (started_++ == 0) {
        threadPool_->Start();
        if (idleTimeoutSec_ > 0.0) {
            StartCleanupTimer();
        }
        loop_->RunInLoop(std::bind(&Acceptor::Listen, acceptor_.get()));
    }
}

void TcpServer::StartCleanupTimer() {
    if (cleanupTimerFd_ >= 0) return;

    cleanupTimerFd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (cleanupTimerFd_ < 0) {
        LOG_ERROR << "TcpServer timerfd_create failed errno=" << errno;
        return;
    }

    cleanupTimerChannel_ = std::make_shared<proxy::network::Channel>(loop_, cleanupTimerFd_);
    cleanupTimerChannel_->SetReadCallback([this](std::chrono::system_clock::time_point) {
        uint64_t one;
        ::read(cleanupTimerFd_, &one, sizeof one);
        CleanupIdleConnections();
    });
    cleanupTimerChannel_->EnableReading();

    struct itimerspec howlong;
    std::memset(&howlong, 0, sizeof howlong);
    const long sec = static_cast<long>(cleanupIntervalSec_);
    const long nsec = static_cast<long>((cleanupIntervalSec_ - sec) * 1e9);
    howlong.it_interval.tv_sec = (sec > 0) ? sec : 0;
    howlong.it_interval.tv_nsec = (nsec > 0) ? nsec : 1000000; // at least 1ms
    howlong.it_value = howlong.it_interval;
    if (::timerfd_settime(cleanupTimerFd_, 0, &howlong, nullptr) != 0) {
        LOG_ERROR << "TcpServer timerfd_settime failed errno=" << errno;
    }
}

void TcpServer::StopCleanupTimer() {
    if (cleanupTimerChannel_) {
        cleanupTimerChannel_->DisableAll();
        cleanupTimerChannel_->Remove();
        cleanupTimerChannel_.reset();
    }
    if (cleanupTimerFd_ >= 0) {
        ::close(cleanupTimerFd_);
        cleanupTimerFd_ = -1;
    }
}

void TcpServer::CleanupIdleConnections() {
    if (idleTimeoutSec_ <= 0.0) return;

    const auto now = std::chrono::steady_clock::now();
    const auto timeout = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(idleTimeoutSec_));

    std::vector<TcpConnectionPtr> toClose;
    toClose.reserve(connections_.size());

    for (auto const& [name, conn] : connections_) {
        if (!conn) continue;
        const auto last = conn->LastActiveTime();
        if (now - last > timeout) {
            LOG_WARN << "TcpServer::CleanupIdleConnections [" << this->name_ << "] closing idle conn "
                     << name << " peer=" << conn->peerAddress().toIpPort();
            toClose.push_back(conn);
        }
    }

    for (auto& conn : toClose) {
        conn->ForceClose();
    }
}

void TcpServer::NewConnection(int sockfd, const InetAddress& peerAddr) {
    const std::string peerIp = peerAddr.toIp();
    if (acceptRateLimiter_ && !acceptRateLimiter_->Allow(1.0)) {
        LOG_WARN << "TcpServer::NewConnection [" << name_ << "] drop (accept rate): " << peerAddr.toIpPort();
        ::close(sockfd);
        proxy::monitor::Stats::Instance().AddDdosDrops(1);
        return;
    }
    if (perIpAcceptLimiter_ && !perIpAcceptLimiter_->Allow(peerIp)) {
        LOG_WARN << "TcpServer::NewConnection [" << name_ << "] drop (per-ip accept rate): " << peerAddr.toIpPort();
        ::close(sockfd);
        proxy::monitor::Stats::Instance().AddDdosDrops(1);
        return;
    }
    const int currentTotal = static_cast<int>(connections_.size());
    if (maxConnections_ > 0 && currentTotal >= maxConnections_) {
        LOG_WARN << "TcpServer::NewConnection [" << name_ << "] reject (max total reached): "
                 << peerAddr.toIpPort() << " total=" << currentTotal << " limit=" << maxConnections_;
        ::close(sockfd);
        return;
    }
    if (maxConnectionsPerIp_ > 0) {
        const int currentIp = ipConnectionCounts_[peerIp];
        if (currentIp >= maxConnectionsPerIp_) {
            LOG_WARN << "TcpServer::NewConnection [" << name_ << "] reject (max per-ip reached): "
                     << peerAddr.toIpPort() << " ip_count=" << currentIp << " limit=" << maxConnectionsPerIp_;
            ::close(sockfd);
            return;
        }
    }

    char buf[64];
    snprintf(buf, sizeof buf, "-%s#%d", hostport_.c_str(), next_conn_id_);
    ++next_conn_id_;
    std::string connName = name_ + buf;

    LOG_INFO << "TcpServer::NewConnection [" << name_ << "] - new connection [" << connName << "] from " << peerAddr.toIpPort();

    EventLoop* ioLoop = threadPool_->GetNextLoop(); 
    
    InetAddress localAddr(0); // TODO
    TcpConnectionPtr conn(new TcpConnection(ioLoop,
                                            connName,
                                            sockfd,
                                            localAddr,
                                            peerAddr,
                                            tlsCtx_ ? tlsCtx_->ctx() : nullptr));
    connections_[connName] = conn;
    ipConnectionCounts_[peerIp] += 1;
    conn->SetConnectionCallback(connectionCallback_);
    conn->SetMessageCallback(messageCallback_);
    conn->SetWriteCompleteCallback(writeCompleteCallback_);
    conn->SetCloseCallback(
        std::bind(&TcpServer::RemoveConnection, this, std::placeholders::_1));
    
    ioLoop->RunInLoop(std::bind(&TcpConnection::ConnectEstablished, conn));
}

void TcpServer::RemoveConnection(const TcpConnectionPtr& conn) {
    // Always defer removal to avoid re-entrancy inside TcpConnection event callbacks.
    loop_->QueueInLoop(std::bind(&TcpServer::RemoveConnectionInLoop, this, conn));
}

void TcpServer::RemoveConnectionInLoop(const TcpConnectionPtr& conn) {
    LOG_INFO << "TcpServer::RemoveConnectionInLoop [" << name_ << "] - connection " << conn->name();
    size_t n = connections_.erase(conn->name());
    (void)n;
    // assert(n == 1);

    const std::string peerIp = conn->peerAddress().toIp();
    auto it = ipConnectionCounts_.find(peerIp);
    if (it != ipConnectionCounts_.end()) {
        it->second -= 1;
        if (it->second <= 0) {
            ipConnectionCounts_.erase(it);
        }
    }

    EventLoop* ioLoop = conn->getLoop();
    ioLoop->QueueInLoop(
        std::bind(&TcpConnection::ConnectDestroyed, conn));
}

} // namespace network
} // namespace proxy
