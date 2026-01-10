#include "proxy/network/UdpProxyServer.h"
#include "proxy/common/Logger.h"
#include "proxy/monitor/Stats.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <cstring>
#include <cstdint>
#include <vector>

namespace proxy {
namespace network {

static int CreateNonblockingUdpSocketOrDie() {
    int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_UDP);
    if (fd < 0) {
        LOG_FATAL << "UdpProxyServer socket(AF_INET,SOCK_DGRAM)";
    }
    return fd;
}

UdpProxyServer::UdpProxyServer(EventLoop* loop, uint16_t listenPort, const std::string& name)
    : loop_(loop),
      name_(name),
      listenFd_(CreateNonblockingUdpSocketOrDie()),
      listenChannel_(new Channel(loop, listenFd_)),
      backendManager_(loop, "roundrobin") {

    int opt = 1;
    ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_RXQ_OVFL
    ::setsockopt(listenFd_, SOL_SOCKET, SO_RXQ_OVFL, &opt, sizeof(opt));
#endif

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(listenPort);
    if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        LOG_FATAL << "UdpProxyServer::bind";
    }

    listenChannel_->SetReadCallback(std::bind(&UdpProxyServer::OnClientReadable, this, std::placeholders::_1));
}

UdpProxyServer::~UdpProxyServer() {
    StopCleanupTimer();

    if (listenChannel_) {
        listenChannel_->DisableAll();
        listenChannel_->Remove();
    }

    for (auto& [key, session] : sessions_) {
        if (session.channel) {
            session.channel->DisableAll();
            session.channel->Remove();
        }
        if (session.fd >= 0) {
            ::close(session.fd);
            session.fd = -1;
        }
    }
    sessions_.clear();
    fdToClientKey_.clear();

    if (listenFd_ >= 0) {
        ::close(listenFd_);
        listenFd_ = -1;
    }
}

void UdpProxyServer::SetIdleTimeout(double idleTimeoutSec, double cleanupIntervalSec) {
    idleTimeoutSec_ = idleTimeoutSec;
    cleanupIntervalSec_ = (cleanupIntervalSec > 0.0) ? cleanupIntervalSec : 1.0;
}

void UdpProxyServer::Start() {
    LOG_INFO << name_ << " starts listening (UDP fd=" << listenFd_ << ")";
    listenChannel_->EnableReading();
    StartCleanupTimer();
}

std::string UdpProxyServer::ClientKey(const sockaddr_in& addr) const {
    char ip[64] = "";
    ::inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    const uint16_t port = ntohs(addr.sin_port);
    std::string key(ip);
    key.push_back(':');
    key += std::to_string(port);
    return key;
}

UdpProxyServer::Session* UdpProxyServer::GetOrCreateSession(const sockaddr_in& clientAddr) {
    const std::string key = ClientKey(clientAddr);
    auto it = sessions_.find(key);
    if (it != sessions_.end()) {
        it->second.lastActive = std::chrono::steady_clock::now();
        return &it->second;
    }

    // Choose backend based on client key (stable hash)
    InetAddress backend = backendManager_.SelectBackend(key);
    if (backend.toIpPort() == "0.0.0.0:0") {
        return nullptr;
    }

    Session session;
    session.clientAddr = clientAddr;
    session.backendAddr = backend;
    session.lastActive = std::chrono::steady_clock::now();

    session.fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_UDP);
    if (session.fd < 0) {
        LOG_ERROR << "UdpProxyServer create session socket failed errno=" << errno;
        return nullptr;
    }
#ifdef SO_RXQ_OVFL
    {
        int opt = 1;
        ::setsockopt(session.fd, SOL_SOCKET, SO_RXQ_OVFL, &opt, sizeof(opt));
    }
#endif
    if (::connect(session.fd, backend.getSockAddr(), sizeof(sockaddr_in)) != 0) {
        if (errno != EINPROGRESS) {
            LOG_ERROR << "UdpProxyServer connect backend failed errno=" << errno;
            ::close(session.fd);
            return nullptr;
        }
    }

    int fd = session.fd;
    session.channel.reset(new Channel(loop_, fd));
    session.channel->SetReadCallback([this, fd](std::chrono::system_clock::time_point t) {
        this->OnBackendReadable(fd, t);
    });
    session.channel->EnableReading();

    fdToClientKey_[fd] = key;
    auto [insIt, ok] = sessions_.emplace(key, std::move(session));
    (void)ok;
    return &insIt->second;
}

void UdpProxyServer::CloseSession(const std::string& key) {
    auto it = sessions_.find(key);
    if (it == sessions_.end()) return;
    auto& session = it->second;
    if (session.channel) {
        session.channel->DisableAll();
        session.channel->Remove();
        session.channel.reset();
    }
    if (session.fd >= 0) {
        fdToClientKey_.erase(session.fd);
        ::close(session.fd);
        session.fd = -1;
    }
    sessions_.erase(it);
}

void UdpProxyServer::OnClientReadable(std::chrono::system_clock::time_point) {
    for (;;) {
        char buf[65536];
        sockaddr_in clientAddr;
        socklen_t len = sizeof(clientAddr);
        std::memset(&clientAddr, 0, sizeof(clientAddr));

        iovec iov;
        iov.iov_base = buf;
        iov.iov_len = sizeof(buf);
        char control[128];
        msghdr msg;
        std::memset(&msg, 0, sizeof(msg));
        msg.msg_name = &clientAddr;
        msg.msg_namelen = len;
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);

        ssize_t n = ::recvmsg(listenFd_, &msg, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            LOG_ERROR << "UdpProxyServer recvfrom failed errno=" << errno;
            break;
        }
        if (n == 0) {
            // UDP: ignore
            continue;
        }
#ifdef SO_RXQ_OVFL
        for (cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
            if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SO_RXQ_OVFL) {
                std::uint32_t dropped = 0;
                std::memcpy(&dropped, CMSG_DATA(cmsg), sizeof(dropped));
                if (dropped > 0) {
                    proxy::monitor::Stats::Instance().AddUdpRxDrops(static_cast<long long>(dropped));
                }
            }
        }
#endif

        Session* session = GetOrCreateSession(clientAddr);
        if (!session) continue;

        ssize_t s = ::send(session->fd, buf, static_cast<size_t>(n), 0);
        if (s < 0) {
            LOG_WARN << "UdpProxyServer send to backend failed errno=" << errno;
        }
        session->lastActive = std::chrono::steady_clock::now();
    }
}

void UdpProxyServer::OnBackendReadable(int sessionFd, std::chrono::system_clock::time_point) {
    auto itKey = fdToClientKey_.find(sessionFd);
    if (itKey == fdToClientKey_.end()) return;
    auto it = sessions_.find(itKey->second);
    if (it == sessions_.end()) return;
    Session& session = it->second;

    for (;;) {
        char buf[65536];
        iovec iov;
        iov.iov_base = buf;
        iov.iov_len = sizeof(buf);
        char control[128];
        msghdr msg;
        std::memset(&msg, 0, sizeof(msg));
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);

        ssize_t n = ::recvmsg(sessionFd, &msg, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            LOG_ERROR << "UdpProxyServer recv backend failed errno=" << errno;
            break;
        }
        if (n == 0) break;
#ifdef SO_RXQ_OVFL
        for (cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
            if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SO_RXQ_OVFL) {
                std::uint32_t dropped = 0;
                std::memcpy(&dropped, CMSG_DATA(cmsg), sizeof(dropped));
                if (dropped > 0) {
                    proxy::monitor::Stats::Instance().AddUdpRxDrops(static_cast<long long>(dropped));
                }
            }
        }
#endif

        ssize_t s = ::sendto(listenFd_, buf, static_cast<size_t>(n), 0,
                             reinterpret_cast<sockaddr*>(&session.clientAddr),
                             sizeof(sockaddr_in));
        if (s < 0) {
            LOG_WARN << "UdpProxyServer sendto client failed errno=" << errno;
        }
        session.lastActive = std::chrono::steady_clock::now();
    }
}

void UdpProxyServer::StartCleanupTimer() {
    if (cleanupTimerFd_ >= 0) return;
    if (idleTimeoutSec_ <= 0.0) return;

    cleanupTimerFd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (cleanupTimerFd_ < 0) {
        LOG_ERROR << "UdpProxyServer timerfd_create failed errno=" << errno;
        return;
    }

    cleanupTimerChannel_.reset(new Channel(loop_, cleanupTimerFd_));
    cleanupTimerChannel_->SetReadCallback(std::bind(&UdpProxyServer::OnCleanupTimerReadable, this, std::placeholders::_1));
    cleanupTimerChannel_->EnableReading();

    struct itimerspec howlong;
    std::memset(&howlong, 0, sizeof howlong);
    const long sec = static_cast<long>(cleanupIntervalSec_);
    const long nsec = static_cast<long>((cleanupIntervalSec_ - sec) * 1e9);
    howlong.it_interval.tv_sec = (sec > 0) ? sec : 0;
    howlong.it_interval.tv_nsec = (nsec > 0) ? nsec : 1000000;
    howlong.it_value = howlong.it_interval;
    ::timerfd_settime(cleanupTimerFd_, 0, &howlong, nullptr);
}

void UdpProxyServer::StopCleanupTimer() {
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

void UdpProxyServer::OnCleanupTimerReadable(std::chrono::system_clock::time_point) {
    uint64_t one;
    ::read(cleanupTimerFd_, &one, sizeof one);
    CleanupIdleSessions();
}

void UdpProxyServer::CleanupIdleSessions() {
    if (idleTimeoutSec_ <= 0.0) return;

    const auto now = std::chrono::steady_clock::now();
    const auto timeout = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(idleTimeoutSec_));

    std::vector<std::string> toClose;
    toClose.reserve(sessions_.size());
    for (const auto& [key, session] : sessions_) {
        if (now - session.lastActive > timeout) {
            toClose.push_back(key);
        }
    }
    for (const auto& key : toClose) {
        CloseSession(key);
    }
}

} // namespace network
} // namespace proxy
