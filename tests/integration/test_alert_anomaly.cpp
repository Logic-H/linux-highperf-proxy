#include "proxy/common/Logger.h"
#include "proxy/monitor/AlertManager.h"
#include "proxy/monitor/Stats.h"
#include "proxy/network/EventLoop.h"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

using proxy::network::EventLoop;

namespace {

static uint16_t bindEphemeralPort(int* listenFdOut) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    int opt = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    assert(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    assert(::listen(fd, 16) == 0);

    socklen_t len = sizeof(addr);
    assert(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    uint16_t port = ntohs(addr.sin_port);
    assert(port != 0);
    *listenFdOut = fd;
    return port;
}

static bool pollReadable(int fd, int timeoutMs) {
    pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN | POLLHUP | POLLERR;
    int ret = ::poll(&pfd, 1, timeoutMs);
    return ret == 1;
}

static std::string recvAllWithTimeout(int fd, int timeoutMs = 2000) {
    std::string out;
    while (pollReadable(fd, timeoutMs)) {
        char buf[4096];
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n > 0) out.append(buf, buf + n);
        else break;
        if (out.size() > 256 * 1024) break;
    }
    return out;
}

} // namespace

int main() {
    proxy::common::Logger::Instance().SetLevel(proxy::common::LogLevel::ERROR);

    int listenFd = -1;
    const uint16_t port = bindEphemeralPort(&listenFd);

    std::atomic<bool> got{false};
    std::string req;

    EventLoop loop;

    std::thread server([&]() {
        if (!pollReadable(listenFd, 3500)) {
            ::close(listenFd);
            loop.QueueInLoop([&]() { loop.Quit(); });
            return;
        }
        int cfd = ::accept(listenFd, nullptr, nullptr);
        if (cfd >= 0) {
            req = recvAllWithTimeout(cfd, 2000);
            const char* resp = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK";
            ::send(cfd, resp, std::strlen(resp), 0);
            ::shutdown(cfd, SHUT_RDWR);
            ::close(cfd);
            got.store(true);
        }
        ::close(listenFd);
        loop.QueueInLoop([&]() { loop.Quit(); });
    });

    auto& stats = proxy::monitor::Stats::Instance();

    proxy::monitor::AlertManager::Config cfg;
    cfg.enabled = true;
    cfg.intervalSec = 0.05;
    cfg.cooldownSec = 0.0;
    cfg.mergeWindowSec = 0.0;
    cfg.webhookUrl = std::string("http://127.0.0.1:") + std::to_string(port) + "/alert";
    cfg.anomaly.enabled = true;
    cfg.anomaly.zThreshold = 2.0;
    cfg.anomaly.alpha = 0.2;
    cfg.anomaly.minSamples = 3;

    proxy::monitor::AlertManager am(&loop, cfg);
    am.Start();

    std::thread feeder([&]() {
        auto addReq = [&](int n) {
            for (int i = 0; i < n; ++i) stats.IncTotalRequests();
        };
        auto addFail = [&](int n) {
            for (int i = 0; i < n; ++i) stats.IncBackendFailures();
        };

        // Build history: several intervals with ~0 error rate.
        for (int i = 0; i < 6; ++i) {
            addReq(50);
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
        }
        // Spike: high error rate in a short interval.
        addReq(50);
        addFail(40);
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
    });

    std::thread killer([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(4500));
        loop.QueueInLoop([&]() { loop.Quit(); });
    });

    loop.Loop();

    feeder.join();
    killer.join();
    server.join();

    assert(got.load());
    assert(req.find("POST /alert HTTP/1.1") != std::string::npos);
    assert(req.find("\"type\":\"anomaly\"") != std::string::npos || req.find("\"type\":\"mixed\"") != std::string::npos);
    assert(req.find("\"metric\":\"backend_error_rate_interval\"") != std::string::npos);
    return 0;
}

