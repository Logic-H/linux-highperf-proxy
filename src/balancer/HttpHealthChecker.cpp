#include "proxy/balancer/HttpHealthChecker.h"
#include "proxy/common/Logger.h"

#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace proxy {
namespace balancer {

HttpHealthChecker::HttpHealthChecker(proxy::network::EventLoop* loop,
                                     double timeoutSec,
                                     std::string hostHeader,
                                     std::string path,
                                     int okStatusMin,
                                     int okStatusMax)
    : HealthChecker(loop),
      timeoutSec_(timeoutSec),
      hostHeader_(std::move(hostHeader)),
      path_(std::move(path)),
      okStatusMin_(okStatusMin),
      okStatusMax_(okStatusMax) {
}

void HttpHealthChecker::Check(const proxy::network::InetAddress& addr, CheckCallback cb) {
    loop_->RunInLoop([this, addr, cb]() {
        int sockfd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
        if (sockfd < 0) {
            LOG_ERROR << "HttpHealthChecker::Check socket error";
            cb(false, addr);
            return;
        }

        int tfd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (tfd < 0) {
            LOG_ERROR << "HttpHealthChecker::Check timerfd error";
            ::close(sockfd);
            cb(false, addr);
            return;
        }

        struct itimerspec howlong;
        bzero(&howlong, sizeof howlong);
        howlong.it_value.tv_sec = static_cast<time_t>(timeoutSec_);
        howlong.it_value.tv_nsec = static_cast<long>((timeoutSec_ - (long)timeoutSec_) * 1000000000);
        ::timerfd_settime(tfd, 0, &howlong, NULL);

        auto ctx = std::make_shared<CheckContext>();
        ctx->sockfd = sockfd;
        ctx->timerfd = tfd;
        ctx->cb = cb;
        ctx->addr = addr;

        ctx->out = "GET " + path_ + " HTTP/1.1\r\n"
                   "Host: " +
                   hostHeader_ +
                   "\r\n"
                   "Connection: close\r\n"
                   "\r\n";

        ctx->connChannel = std::make_shared<proxy::network::Channel>(loop_, sockfd);
        ctx->connChannel->SetWriteCallback([this, ctx]() { OnWritable(ctx); });
        ctx->connChannel->SetReadCallback([this, ctx](std::chrono::system_clock::time_point t) { OnReadable(ctx, t); });

        ctx->timerChannel = std::make_shared<proxy::network::Channel>(loop_, tfd);
        ctx->timerChannel->SetReadCallback([this, ctx](std::chrono::system_clock::time_point) { OnTimeout(ctx); });
        ctx->timerChannel->EnableReading();

        int ret = ::connect(sockfd, addr.getSockAddr(), sizeof(struct sockaddr_in));
        int savedErrno = (ret == 0) ? 0 : errno;
        if (ret == 0 || savedErrno == EISCONN) {
            ctx->state = State::kSending;
            OnWritable(ctx);
        } else if (savedErrno == EINPROGRESS) {
            ctx->state = State::kConnecting;
            ctx->connChannel->EnableWriting();
        } else {
            ::close(sockfd);
            ::close(tfd);
            ctx->timerChannel->DisableAll();
            ctx->timerChannel->Remove();
            cb(false, addr);
        }
    });
}

void HttpHealthChecker::OnWritable(std::shared_ptr<CheckContext> ctx) {
    if (ctx->finished.load()) return;

    if (ctx->state == State::kConnecting) {
        int err = 0;
        socklen_t len = sizeof err;
        if (::getsockopt(ctx->sockfd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) {
            err = errno;
        }
        if (err) {
            OnError(ctx);
            return;
        }
        ctx->state = State::kSending;
    }

    if (ctx->state == State::kSending) {
        while (ctx->outOffset < ctx->out.size()) {
            const char* p = ctx->out.data() + ctx->outOffset;
            const size_t left = ctx->out.size() - ctx->outOffset;
            const ssize_t n = ::send(ctx->sockfd, p, left, MSG_NOSIGNAL);
            if (n > 0) {
                ctx->outOffset += static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                ctx->connChannel->EnableWriting();
                return;
            }
            OnError(ctx);
            return;
        }

        // Sent all.
        ctx->connChannel->DisableWriting();
        ctx->state = State::kReading;
        ctx->connChannel->EnableReading();
        return;
    }
}

void HttpHealthChecker::OnReadable(std::shared_ptr<CheckContext> ctx, std::chrono::system_clock::time_point) {
    if (ctx->finished.load()) return;

    char buf[4096];
    while (true) {
        const ssize_t n = ::recv(ctx->sockfd, buf, sizeof(buf), 0);
        if (n > 0) {
            ctx->in.append(buf, buf + n);
            const size_t pos = ctx->in.find("\r\n");
            if (pos != std::string::npos) {
                const int code = ParseHttpStatusCode(ctx->in.substr(0, pos));
                const bool ok = (code >= okStatusMin_ && code <= okStatusMax_);
                if (CleanUp(ctx)) {
                    if (ctx->cb) ctx->cb(ok, ctx->addr);
                }
                return;
            }
            continue;
        }
        if (n == 0) {
            // EOF before status line
            if (CleanUp(ctx)) {
                if (ctx->cb) ctx->cb(false, ctx->addr);
            }
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        OnError(ctx);
        return;
    }
}

void HttpHealthChecker::OnError(std::shared_ptr<CheckContext> ctx) {
    if (CleanUp(ctx)) {
        if (ctx->cb) ctx->cb(false, ctx->addr);
    }
}

void HttpHealthChecker::OnTimeout(std::shared_ptr<CheckContext> ctx) {
    uint64_t one;
    ssize_t n = ::read(ctx->timerfd, &one, sizeof one);
    (void)n;
    if (CleanUp(ctx)) {
        if (ctx->cb) ctx->cb(false, ctx->addr);
    }
}

bool HttpHealthChecker::CleanUp(std::shared_ptr<CheckContext> ctx) {
    if (ctx->finished.exchange(true)) return false;

    if (ctx->connChannel) {
        // If current events include both READ/WRITE, Channel::HandleEvent may keep
        // invoking callbacks after we remove it. Clear callbacks defensively.
        ctx->connChannel->SetReadCallback({});
        ctx->connChannel->SetWriteCallback({});
        ctx->connChannel->SetCloseCallback({});
        ctx->connChannel->SetErrorCallback({});
        ctx->connChannel->DisableAll();
        ctx->connChannel->Remove();
    }
    if (ctx->timerChannel) {
        ctx->timerChannel->SetReadCallback({});
        ctx->timerChannel->SetWriteCallback({});
        ctx->timerChannel->SetCloseCallback({});
        ctx->timerChannel->SetErrorCallback({});
        ctx->timerChannel->DisableAll();
        ctx->timerChannel->Remove();
    }

    if (ctx->sockfd >= 0) {
        ::close(ctx->sockfd);
        ctx->sockfd = -1;
    }
    if (ctx->timerfd >= 0) {
        ::close(ctx->timerfd);
        ctx->timerfd = -1;
    }

    ctx->connChannel.reset();
    ctx->timerChannel.reset();
    return true;
}

int HttpHealthChecker::ParseHttpStatusCode(const std::string& line) {
    // Expected: HTTP/1.1 200 OK
    const size_t sp1 = line.find(' ');
    if (sp1 == std::string::npos) return -1;
    const size_t sp2 = line.find(' ', sp1 + 1);
    const std::string codeStr = (sp2 == std::string::npos) ? line.substr(sp1 + 1) : line.substr(sp1 + 1, sp2 - sp1 - 1);
    if (codeStr.size() != 3) return -1;
    int code = 0;
    for (char c : codeStr) {
        if (c < '0' || c > '9') return -1;
        code = code * 10 + (c - '0');
    }
    return code;
}

} // namespace balancer
} // namespace proxy
