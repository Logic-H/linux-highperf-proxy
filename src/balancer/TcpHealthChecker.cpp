#include "proxy/balancer/TcpHealthChecker.h"
#include "proxy/common/Logger.h"
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <cerrno>

namespace proxy {
namespace balancer {

TcpHealthChecker::TcpHealthChecker(proxy::network::EventLoop* loop, double timeoutSec)
    : HealthChecker(loop), timeoutSec_(timeoutSec) {
}

void TcpHealthChecker::Check(const proxy::network::InetAddress& addr, CheckCallback cb) {
    auto* loop = loop_;
    const double timeoutSec = timeoutSec_;
    loop_->RunInLoop([loop, timeoutSec, addr, cb]() {
        int sockfd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
        if (sockfd < 0) {
            LOG_ERROR << "TcpHealthChecker::Check socket error";
            cb(false, addr);
            return;
        }

        // Create timer fd
        int tfd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (tfd < 0) {
            LOG_ERROR << "TcpHealthChecker::Check timerfd error";
            ::close(sockfd);
            cb(false, addr);
            return;
        }

        struct itimerspec howlong;
        bzero(&howlong, sizeof howlong);
        howlong.it_value.tv_sec = static_cast<time_t>(timeoutSec);
        howlong.it_value.tv_nsec = static_cast<long>((timeoutSec - (long)timeoutSec) * 1000000000);
        ::timerfd_settime(tfd, 0, &howlong, NULL);

        auto ctx = std::make_shared<CheckContext>();
        ctx->loop = loop;
        ctx->sockfd = sockfd;
        ctx->timerfd = tfd;
        ctx->cb = cb;
        ctx->addr = addr;
        
        // Setup Connection Channel
        ctx->connChannel = std::make_shared<proxy::network::Channel>(loop, sockfd);
        ctx->connChannel->SetWriteCallback([ctx]() { OnConnected(ctx); });
        ctx->connChannel->SetErrorCallback([ctx]() { OnError(ctx); });

        // Setup Timer Channel
        ctx->timerChannel = std::make_shared<proxy::network::Channel>(loop, tfd);
        ctx->timerChannel->SetReadCallback(
            [ctx](std::chrono::system_clock::time_point) { OnTimeout(ctx); });
        ctx->timerChannel->EnableReading();

        // Connect
        int ret = ::connect(sockfd, addr.getSockAddr(), sizeof(struct sockaddr_in));
        int savedErrno = (ret == 0) ? 0 : errno;

        if (ret == 0 || savedErrno == EISCONN) {
            // Instant connection
            OnConnected(ctx);
        } else if (savedErrno == EINPROGRESS) {
            ctx->connChannel->EnableWriting();
        } else {
            // LOG_ERROR << "TcpHealthChecker connect error: " << savedErrno;
            // Immediate failure
            ::close(sockfd);
            ::close(tfd);
            ctx->timerChannel->DisableAll();
            ctx->timerChannel->Remove();
            cb(false, addr);
        }
    });
}

void TcpHealthChecker::OnConnected(std::shared_ptr<CheckContext> ctx) {
    if (ctx->finished.load()) return;

    // Check SO_ERROR
    int err = 0;
    socklen_t len = sizeof err;
    if (::getsockopt(ctx->sockfd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) {
        err = errno;
    }

    if (err) {
        // LOG_WARN << "TcpHealthChecker connect SO_ERROR=" << err;
        OnError(ctx);
        return;
    }

    if (CleanUp(ctx)) {
        if (ctx->cb) ctx->cb(true, ctx->addr);
    }
}

void TcpHealthChecker::OnError(std::shared_ptr<CheckContext> ctx) {
    if (CleanUp(ctx)) {
        if (ctx->cb) ctx->cb(false, ctx->addr);
    }
}

void TcpHealthChecker::OnTimeout(std::shared_ptr<CheckContext> ctx) {
    uint64_t one;
    ssize_t n = ::read(ctx->timerfd, &one, sizeof one);
    (void)n;
    
    // LOG_WARN << "TcpHealthChecker timeout for " << ctx->addr.toIpPort();
    
    if (CleanUp(ctx)) {
        if (ctx->cb) ctx->cb(false, ctx->addr);
    }
}

bool TcpHealthChecker::CleanUp(std::shared_ptr<CheckContext> ctx) {
    if (ctx->finished.exchange(true)) {
        return false; // Already cleaned up
    }

    auto* loop = ctx->loop;
    const int sockfd = ctx->sockfd;
    const int timerfd = ctx->timerfd;
    auto connHold = std::move(ctx->connChannel);
    auto timerHold = std::move(ctx->timerChannel);

    if (connHold) {
        connHold->DisableAll();
        connHold->Remove();
    }
    if (timerHold) {
        timerHold->DisableAll();
        timerHold->Remove();
    }
    
    // Defer fd close and Channel destruction until after the current poll cycle,
    // otherwise EventLoop may still hold raw Channel pointers in active_channels_.
    loop->QueueInLoop([connHold, timerHold, sockfd, timerfd]() mutable {
        ::close(sockfd);
        ::close(timerfd);
        (void)connHold;
        (void)timerHold;
    });
    
    return true;
}

} // namespace balancer
} // namespace proxy
