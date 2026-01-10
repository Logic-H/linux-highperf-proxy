#include "proxy/balancer/BackendManager.h"
#include "proxy/common/Logger.h"
#include "proxy/network/EventLoop.h"
#include "proxy/network/Channel.h"

#include <arpa/inet.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

using proxy::network::EventLoop;

namespace {

static bool pollReadable(int fd, int timeoutMs) {
    pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN | POLLHUP | POLLERR;
    int ret = ::poll(&pfd, 1, timeoutMs);
    return ret == 1;
}

static void sendAll(int fd, const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
        ssize_t n = ::send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
        if (n <= 0) return;
        off += static_cast<size_t>(n);
    }
}

static std::optional<uint16_t> bindEphemeralPort(int* listenFdOut) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return std::nullopt;
    int opt = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return std::nullopt;
    }
    if (::listen(fd, 16) != 0) {
        ::close(fd);
        return std::nullopt;
    }
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        ::close(fd);
        return std::nullopt;
    }
    const uint16_t port = ntohs(addr.sin_port);
    if (port == 0) {
        ::close(fd);
        return std::nullopt;
    }
    *listenFdOut = fd;
    return port;
}

static void warmupBackend(int lfd, std::atomic<bool>* stop) {
    while (!stop->load()) {
        if (!pollReadable(lfd, 200)) continue;
        int cfd = ::accept(lfd, nullptr, nullptr);
        if (cfd < 0) continue;

        std::string in;
        auto start = std::chrono::steady_clock::now();
        while (!stop->load()) {
            if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() > 2000) break;
            if (!pollReadable(cfd, 200)) continue;
            char buf[4096];
            ssize_t n = ::recv(cfd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            in.append(buf, buf + n);
            if (in.find("\r\n\r\n") != std::string::npos) break;
        }

        bool ok = (in.find("POST /ai/warmup?model=m1") != std::string::npos);
        const std::string body = ok ? "OK" : "NO";
        const std::string resp = std::string("HTTP/1.1 ") + (ok ? "200 OK" : "404 Not Found") + "\r\n" +
                                 "Content-Length: " + std::to_string(body.size()) + "\r\n" +
                                 "Connection: close\r\n" +
                                 "\r\n" + body;
        sendAll(cfd, resp);
        ::shutdown(cfd, SHUT_RDWR);
        ::close(cfd);
    }
    ::close(lfd);
}

} // namespace

int main() {
    ::signal(SIGPIPE, SIG_IGN);
    proxy::common::Logger::Instance().SetLevel(proxy::common::LogLevel::ERROR);

    int lfd = -1;
    auto portOpt = bindEphemeralPort(&lfd);
    if (!portOpt) {
        std::cerr << "Failed to bind backend port\n";
        if (lfd >= 0) ::close(lfd);
        return 1;
    }
    const uint16_t port = *portOpt;

    std::atomic<bool> stop{false};
    std::thread backend([&]() { warmupBackend(lfd, &stop); });

    EventLoop loop;
    proxy::balancer::BackendManager mgr(&loop, "roundrobin");
    mgr.ConfigureWarmup(true, "m1", 1.0, "127.0.0.1", "/ai/warmup");
    mgr.AddBackend("127.0.0.1", port, 1);

    // Before warmup completes, backend should not be eligible.
    assert(mgr.SelectBackend("k").toIpPort() == "0.0.0.0:0");

    int tfd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    assert(tfd >= 0);
    itimerspec ts;
    std::memset(&ts, 0, sizeof(ts));
    ts.it_value.tv_sec = 0;
    ts.it_value.tv_nsec = 300 * 1000 * 1000L;
    ::timerfd_settime(tfd, 0, &ts, nullptr);

    auto ch = std::make_shared<proxy::network::Channel>(&loop, tfd);
    ch->SetReadCallback([&](std::chrono::system_clock::time_point) {
        uint64_t one = 0;
        ::read(tfd, &one, sizeof(one));
        const std::string picked = mgr.SelectBackend("k").toIpPort();
        assert(picked == std::string("127.0.0.1:") + std::to_string(port));
        loop.Quit();
    });
    ch->EnableReading();

    loop.Loop();

    ch->DisableAll();
    ch->Remove();
    ::close(tfd);

    stop.store(true);
    backend.join();
    return 0;
}

