#include "proxy/balancer/AiServiceChecker.h"
#include "proxy/common/Logger.h"

#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <cerrno>
#include <cctype>
#include <cstring>
#include <sstream>

namespace proxy {
namespace balancer {

AiServiceChecker::AiServiceChecker(proxy::network::EventLoop* loop,
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

void AiServiceChecker::Check(const proxy::network::InetAddress& addr, CheckCallback cb) {
    loop_->RunInLoop([self = shared_from_this(), addr, cb]() {
        int sockfd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
        if (sockfd < 0) {
            LOG_ERROR << "AiServiceChecker::Check socket error";
            if (cb) cb(false, addr, Result{});
            return;
        }

        int tfd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (tfd < 0) {
            LOG_ERROR << "AiServiceChecker::Check timerfd error";
            ::close(sockfd);
            if (cb) cb(false, addr, Result{});
            return;
        }

        struct itimerspec howlong;
        bzero(&howlong, sizeof howlong);
        howlong.it_value.tv_sec = static_cast<time_t>(self->timeoutSec_);
        howlong.it_value.tv_nsec = static_cast<long>((self->timeoutSec_ - (long)self->timeoutSec_) * 1000000000);
        ::timerfd_settime(tfd, 0, &howlong, NULL);

        auto ctx = std::make_shared<CheckContext>();
        ctx->sockfd = sockfd;
        ctx->timerfd = tfd;
        ctx->cb = cb;
        ctx->addr = addr;

        ctx->out = "GET " + self->path_ + " HTTP/1.1\r\n"
                   "Host: " +
                   self->hostHeader_ +
                   "\r\n"
                   "Connection: close\r\n"
                   "\r\n";

        ctx->connChannel = std::make_shared<proxy::network::Channel>(self->loop_, sockfd);
        ctx->timerChannel = std::make_shared<proxy::network::Channel>(self->loop_, tfd);

        ctx->connChannel->SetWriteCallback([self, ctx]() { self->OnWritable(ctx); });
        ctx->connChannel->SetReadCallback([self, ctx](std::chrono::system_clock::time_point t) { self->OnReadable(ctx, t); });
        ctx->connChannel->SetErrorCallback([self, ctx]() { self->OnError(ctx); });

        ctx->timerChannel->SetReadCallback([self, ctx](std::chrono::system_clock::time_point) { self->OnTimeout(ctx); });
        ctx->timerChannel->EnableReading();

        const int ret = ::connect(sockfd, (struct sockaddr*)addr.getSockAddr(), sizeof(struct sockaddr_in));
        if (ret == 0) {
            ctx->state = State::kSending;
            self->OnWritable(ctx);
            return;
        }
        if (ret < 0 && (errno == EINPROGRESS || errno == EALREADY)) {
            ctx->state = State::kConnecting;
            ctx->connChannel->EnableWriting();
            return;
        }

        self->OnError(ctx);
    });
}

void AiServiceChecker::OnWritable(std::shared_ptr<CheckContext> ctx) {
    if (ctx->finished.load()) return;

    if (ctx->state == State::kConnecting) {
        int err = 0;
        socklen_t len = sizeof err;
        if (::getsockopt(ctx->sockfd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
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

        ctx->connChannel->DisableWriting();
        ctx->state = State::kReading;
        ctx->connChannel->EnableReading();
        return;
    }
}

void AiServiceChecker::OnReadable(std::shared_ptr<CheckContext> ctx, std::chrono::system_clock::time_point) {
    if (ctx->finished.load()) return;

    char buf[4096];
    while (true) {
        const ssize_t n = ::recv(ctx->sockfd, buf, sizeof(buf), 0);
        if (n > 0) {
            ctx->in.append(buf, buf + n);
            if (ctx->in.size() > 32768) {
                OnError(ctx);
                return;
            }
            continue;
        }
        if (n == 0) {
            // EOF -> parse response
            Result r;
            bool ok = false;
            const size_t lineEnd = ctx->in.find("\r\n");
            if (lineEnd != std::string::npos) {
                const int code = ParseHttpStatusCode(ctx->in.substr(0, lineEnd));
                const bool codeOk = (code >= okStatusMin_ && code <= okStatusMax_);
                const size_t hdrEnd = ctx->in.find("\r\n\r\n");
                if (codeOk && hdrEnd != std::string::npos) {
                    const std::string body = ctx->in.substr(hdrEnd + 4);
                    ok = ParseJsonResult(body, &r);
                }
            }
            if (CleanUp(ctx)) {
                if (ctx->cb) ctx->cb(ok, ctx->addr, r);
            }
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        OnError(ctx);
        return;
    }
}

void AiServiceChecker::OnError(std::shared_ptr<CheckContext> ctx) {
    if (CleanUp(ctx)) {
        if (ctx->cb) ctx->cb(false, ctx->addr, Result{});
    }
}

void AiServiceChecker::OnTimeout(std::shared_ptr<CheckContext> ctx) {
    uint64_t one;
    ssize_t n = ::read(ctx->timerfd, &one, sizeof one);
    (void)n;
    if (CleanUp(ctx)) {
        if (ctx->cb) ctx->cb(false, ctx->addr, Result{});
    }
}

bool AiServiceChecker::CleanUp(std::shared_ptr<CheckContext> ctx) {
    if (ctx->finished.exchange(true)) return false;

    if (ctx->connChannel) {
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

int AiServiceChecker::ParseHttpStatusCode(const std::string& line) {
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

static bool FindKeyPos(const std::string& s, const std::string& key, size_t* posOut) {
    const std::string pat = "\"" + key + "\"";
    size_t pos = s.find(pat);
    if (pos == std::string::npos) return false;
    pos = s.find(':', pos + pat.size());
    if (pos == std::string::npos) return false;
    *posOut = pos + 1;
    return true;
}

static void SkipSpaces(const std::string& s, size_t* p) {
    while (*p < s.size() && std::isspace(static_cast<unsigned char>(s[*p]))) (*p)++;
}

static bool ParseDouble(const std::string& s, size_t p, double* out) {
    SkipSpaces(s, &p);
    size_t end = p;
    while (end < s.size() && (std::isdigit(static_cast<unsigned char>(s[end])) || s[end] == '.' || s[end] == '-' || s[end] == '+'
                              || s[end] == 'e' || s[end] == 'E')) {
        end++;
    }
    if (end == p) return false;
    try {
        *out = std::stod(s.substr(p, end - p));
        return true;
    } catch (...) {
        return false;
    }
}

static bool ParseInt(const std::string& s, size_t p, int* out) {
    double d = 0.0;
    if (!ParseDouble(s, p, &d)) return false;
    *out = static_cast<int>(d);
    return true;
}

static bool ParseBool(const std::string& s, size_t p, bool* out) {
    SkipSpaces(s, &p);
    if (s.compare(p, 4, "true") == 0) {
        *out = true;
        return true;
    }
    if (s.compare(p, 5, "false") == 0) {
        *out = false;
        return true;
    }
    int v = 0;
    if (ParseInt(s, p, &v)) {
        *out = (v != 0);
        return true;
    }
    return false;
}

static bool ParseJsonString(const std::string& s, size_t p, std::string* out) {
    if (!out) return false;
    SkipSpaces(s, &p);
    if (p >= s.size() || s[p] != '"') return false;
    p++;
    std::string r;
    r.reserve(32);
    while (p < s.size()) {
        const char c = s[p++];
        if (c == '"') {
            *out = std::move(r);
            return true;
        }
        if (c == '\\' && p < s.size()) {
            const char e = s[p++];
            switch (e) {
                case '"': r.push_back('"'); break;
                case '\\': r.push_back('\\'); break;
                case 'n': r.push_back('\n'); break;
                case 'r': r.push_back('\r'); break;
                case 't': r.push_back('\t'); break;
                default: r.push_back(e); break;
            }
            continue;
        }
        r.push_back(c);
        if (r.size() > 1024) return false;
    }
    return false;
}

bool AiServiceChecker::ParseJsonResult(const std::string& body, Result* out) {
    if (!out) return false;
    Result r;
    bool any = false;

    auto getInt = [&](const std::string& key, int* dst, bool* present) {
        size_t p = 0;
        if (!FindKeyPos(body, key, &p)) return;
        int v = 0;
        if (ParseInt(body, p, &v)) {
            *dst = v;
            *present = true;
            any = true;
        }
    };
    auto getDouble01 = [&](const std::string& key, double* dst, bool* present) {
        size_t p = 0;
        if (!FindKeyPos(body, key, &p)) return;
        double v = 0.0;
        if (ParseDouble(body, p, &v)) {
            if (v < 0.0) v = 0.0;
            if (v > 1.0) v = 1.0;
            *dst = v;
            *present = true;
            any = true;
        }
    };
    auto getBool = [&](const std::string& key, bool* dst, bool* present) {
        size_t p = 0;
        if (!FindKeyPos(body, key, &p)) return;
        bool v = false;
        if (ParseBool(body, p, &v)) {
            *dst = v;
            *present = true;
            any = true;
        }
    };
    auto getString = [&](const std::string& key, std::string* dst, bool* present) {
        size_t p = 0;
        if (!FindKeyPos(body, key, &p)) return;
        std::string v;
        if (ParseJsonString(body, p, &v)) {
            *dst = std::move(v);
            *present = true;
            any = true;
        }
    };

    getInt("queue_len", &r.queueLen, &r.hasQueueLen);
    getDouble01("gpu_util", &r.gpuUtil01, &r.hasGpu);
    if (!r.hasGpu) getDouble01("gpu_util01", &r.gpuUtil01, &r.hasGpu);
    getInt("vram_used_mb", &r.vramUsedMb, &r.hasGpu);
    getInt("vram_total_mb", &r.vramTotalMb, &r.hasGpu);
    getBool("model_loaded", &r.modelLoaded, &r.hasModelLoaded);
    if (!r.hasModelLoaded) getBool("modelLoaded", &r.modelLoaded, &r.hasModelLoaded);
    getString("model", &r.modelName, &r.hasModelName);
    if (!r.hasModelName) getString("model_name", &r.modelName, &r.hasModelName);
    if (!r.hasModelName) getString("loaded_model", &r.modelName, &r.hasModelName);
    getString("model_version", &r.modelVersion, &r.hasModelVersion);
    if (!r.hasModelVersion) getString("version", &r.modelVersion, &r.hasModelVersion);
    if (!r.hasModelVersion) getString("modelVersion", &r.modelVersion, &r.hasModelVersion);

    // If VRAM metrics exist but util missing, still count as GPU-present.
    if ((r.vramTotalMb > 0 || r.vramUsedMb > 0) && !r.hasGpu) r.hasGpu = true;

    *out = r;
    return any;
}

} // namespace balancer
} // namespace proxy
