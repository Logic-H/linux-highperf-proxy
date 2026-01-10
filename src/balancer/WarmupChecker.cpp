#include "proxy/balancer/WarmupChecker.h"
#include "proxy/common/Logger.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <cerrno>
#include <cctype>
#include <cstring>

namespace proxy {
namespace balancer {

WarmupChecker::WarmupChecker(proxy::network::EventLoop* loop,
                             double timeoutSec,
                             std::string hostHeader,
                             std::string path,
                             int okStatusMin,
                             int okStatusMax)
    : loop_(loop),
      timeoutSec_(timeoutSec),
      hostHeader_(std::move(hostHeader)),
      path_(std::move(path)),
      okStatusMin_(okStatusMin),
      okStatusMax_(okStatusMax) {
}

int WarmupChecker::parseStatusCode(const std::string& statusLine) {
    // HTTP/1.1 200 OK
    const size_t sp1 = statusLine.find(' ');
    if (sp1 == std::string::npos) return -1;
    const size_t sp2 = statusLine.find(' ', sp1 + 1);
    const std::string codeStr = (sp2 == std::string::npos) ? statusLine.substr(sp1 + 1) : statusLine.substr(sp1 + 1, sp2 - sp1 - 1);
    if (codeStr.size() != 3) return -1;
    int code = 0;
    for (char c : codeStr) {
        if (c < '0' || c > '9') return -1;
        code = code * 10 + (c - '0');
    }
    return code;
}

std::string WarmupChecker::urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
            c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[(c >> 4) & 0xF]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

void WarmupChecker::Warmup(const proxy::network::InetAddress& addr, const std::string& model, Callback cb) {
    if (!loop_) {
        if (cb) cb(false, addr);
        return;
    }
    loop_->RunInLoop([self = shared_from_this(), addr, model, cb]() {
        int sockfd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
        if (sockfd < 0) {
            if (cb) cb(false, addr);
            return;
        }
        int tfd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (tfd < 0) {
            ::close(sockfd);
            if (cb) cb(false, addr);
            return;
        }

        itimerspec ts;
        std::memset(&ts, 0, sizeof(ts));
        const long sec = static_cast<long>(self->timeoutSec_);
        const double frac = self->timeoutSec_ - static_cast<double>(sec);
        ts.it_value.tv_sec = static_cast<time_t>(sec);
        ts.it_value.tv_nsec = static_cast<long>(frac * 1000000000.0);
        ::timerfd_settime(tfd, 0, &ts, nullptr);

        auto ctx = std::make_shared<Ctx>();
        ctx->sockfd = sockfd;
        ctx->timerfd = tfd;
        ctx->cb = cb;
        ctx->addr = addr;
        ctx->connCh = std::make_shared<proxy::network::Channel>(self->loop_, sockfd);
        ctx->timerCh = std::make_shared<proxy::network::Channel>(self->loop_, tfd);

        std::string path = self->path_;
        if (!model.empty()) {
            const std::string sep = (path.find('?') == std::string::npos) ? "?" : "&";
            path += sep + "model=" + urlEncode(model);
        }
        ctx->out = "POST " + path + " HTTP/1.1\r\n"
                   "Host: " +
                   self->hostHeader_ +
                   "\r\n"
                   "Connection: close\r\n"
                   "Content-Length: 0\r\n"
                   "\r\n";

        ctx->connCh->SetWriteCallback([self, ctx]() { self->onWritable(ctx); });
        ctx->connCh->SetReadCallback([self, ctx](std::chrono::system_clock::time_point) { self->onReadable(ctx); });
        ctx->connCh->SetErrorCallback([self, ctx]() { self->onError(ctx); });
        ctx->connCh->SetCloseCallback([self, ctx]() { self->onError(ctx); });
        ctx->timerCh->SetReadCallback([self, ctx](std::chrono::system_clock::time_point) { self->onTimeout(ctx); });
        ctx->timerCh->EnableReading();

        sockaddr_in sa;
        std::memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons(addr.toPort());
        ::inet_pton(AF_INET, addr.toIp().c_str(), &sa.sin_addr);

        int ret = ::connect(sockfd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
        if (ret == 0) {
            ctx->state = State::kSending;
            ctx->connCh->EnableWriting();
            return;
        }
        if (ret < 0 && errno == EINPROGRESS) {
            ctx->state = State::kConnecting;
            ctx->connCh->EnableWriting();
            return;
        }
        self->onError(ctx);
    });
}

void WarmupChecker::onWritable(std::shared_ptr<Ctx> ctx) {
    if (ctx->finished.load()) return;

    if (ctx->state == State::kConnecting) {
        int err = 0;
        socklen_t len = sizeof(err);
        if (::getsockopt(ctx->sockfd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
            onError(ctx);
            return;
        }
        ctx->state = State::kSending;
    }

    while (ctx->outOff < ctx->out.size()) {
        ssize_t n = ::send(ctx->sockfd, ctx->out.data() + ctx->outOff, ctx->out.size() - ctx->outOff, MSG_NOSIGNAL);
        if (n > 0) {
            ctx->outOff += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        onError(ctx);
        return;
    }

    ctx->connCh->DisableWriting();
    ctx->state = State::kReading;
    ctx->connCh->EnableReading();
}

void WarmupChecker::onReadable(std::shared_ptr<Ctx> ctx) {
    if (ctx->finished.load()) return;
    char buf[4096];
    while (true) {
        ssize_t n = ::recv(ctx->sockfd, buf, sizeof(buf), 0);
        if (n > 0) {
            ctx->in.append(buf, buf + n);
            if (ctx->in.size() > 32768) {
                onError(ctx);
                return;
            }
            continue;
        }
        if (n == 0) {
            bool ok = false;
            const size_t lineEnd = ctx->in.find("\r\n");
            if (lineEnd != std::string::npos) {
                const int code = parseStatusCode(ctx->in.substr(0, lineEnd));
                ok = (code >= okStatusMin_ && code <= okStatusMax_);
            }
            if (cleanup(ctx)) {
                if (ctx->cb) ctx->cb(ok, ctx->addr);
            }
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        onError(ctx);
        return;
    }
}

void WarmupChecker::onError(std::shared_ptr<Ctx> ctx) {
    if (cleanup(ctx)) {
        if (ctx->cb) ctx->cb(false, ctx->addr);
    }
}

void WarmupChecker::onTimeout(std::shared_ptr<Ctx> ctx) {
    uint64_t one = 0;
    ::read(ctx->timerfd, &one, sizeof(one));
    if (cleanup(ctx)) {
        if (ctx->cb) ctx->cb(false, ctx->addr);
    }
}

bool WarmupChecker::cleanup(std::shared_ptr<Ctx> ctx) {
    if (ctx->finished.exchange(true)) return false;
    if (ctx->connCh) {
        ctx->connCh->SetReadCallback({});
        ctx->connCh->SetWriteCallback({});
        ctx->connCh->SetCloseCallback({});
        ctx->connCh->SetErrorCallback({});
        ctx->connCh->DisableAll();
        ctx->connCh->Remove();
    }
    if (ctx->timerCh) {
        ctx->timerCh->SetReadCallback({});
        ctx->timerCh->SetWriteCallback({});
        ctx->timerCh->SetCloseCallback({});
        ctx->timerCh->SetErrorCallback({});
        ctx->timerCh->DisableAll();
        ctx->timerCh->Remove();
    }
    if (ctx->sockfd >= 0) {
        ::close(ctx->sockfd);
        ctx->sockfd = -1;
    }
    if (ctx->timerfd >= 0) {
        ::close(ctx->timerfd);
        ctx->timerfd = -1;
    }
    ctx->connCh.reset();
    ctx->timerCh.reset();
    return true;
}

} // namespace balancer
} // namespace proxy
