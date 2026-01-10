#include "proxy/ProxyServer.h"
#include "proxy/common/Logger.h"
#include "proxy/network/EventLoop.h"

#include <arpa/inet.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
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

static std::string recvSome(int fd, int timeoutMs = 2000) {
    if (!pollReadable(fd, timeoutMs)) return {};
    char buf[4096];
    ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) return {};
    return std::string(buf, buf + n);
}

static void sendAll(int fd, const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
        ssize_t n = ::send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
        if (n <= 0) {
            return;
        }
        off += static_cast<size_t>(n);
    }
}

static int connectTo(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
        ::close(fd);
        return -1;
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
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

static std::optional<uint16_t> reserveFreePort() {
    int fd = -1;
    auto port = bindEphemeralPort(&fd);
    if (fd >= 0) ::close(fd);
    return port;
}

static void httpKeepAliveBackend(int lfd, std::atomic<int>* acceptCnt, std::atomic<int>* reqCnt, std::atomic<bool>* stop) {

    while (!stop->load()) {
        if (!pollReadable(lfd, 200)) continue;
        int cfd = ::accept(lfd, nullptr, nullptr);
        if (cfd < 0) continue;
        acceptCnt->fetch_add(1);

        std::string in;
        while (!stop->load()) {
            if (!pollReadable(cfd, 200)) continue;
            char buf[4096];
            ssize_t n = ::recv(cfd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            std::string chunk(buf, buf + n);
            in += chunk;
            if (in.size() > 256 * 1024) break;
            while (true) {
                size_t headerEnd = in.find("\r\n\r\n");
                if (headerEnd == std::string::npos) break;
                // Very small backend: ignore request parsing; always respond OK and keep alive.
                reqCnt->fetch_add(1);
                const std::string body = "OK";
                std::string resp = "HTTP/1.1 200 OK\r\n"
                                   "Content-Length: 2\r\n"
                                   "Connection: keep-alive\r\n"
                                   "\r\n" +
                                   body;
                sendAll(cfd, resp);
                in.erase(0, headerEnd + 4);
            }
        }

        ::shutdown(cfd, SHUT_RDWR);
        ::close(cfd);
    }

    ::close(lfd);
}

} // namespace

int main() {
    ::signal(SIGPIPE, SIG_IGN);
    proxy::common::Logger::Instance().SetLevel(proxy::common::LogLevel::INFO);

    std::atomic<int> acceptCnt{0};
    std::atomic<int> reqCnt{0};
    std::atomic<bool> stopBackend{false};

    int backendListenFd = -1;
    const auto backendPortOpt = bindEphemeralPort(&backendListenFd);
    const auto proxyPortOpt = reserveFreePort();
    if (!backendPortOpt || !proxyPortOpt) {
        std::cerr << "Failed to allocate ephemeral ports\n";
        if (backendListenFd >= 0) ::close(backendListenFd);
        return 1;
    }
    const uint16_t backendPort = *backendPortOpt;
    const uint16_t proxyPort = *proxyPortOpt;

    std::thread backend([&]() { httpKeepAliveBackend(backendListenFd, &acceptCnt, &reqCnt, &stopBackend); });

    EventLoop loop;
    proxy::ProxyServer server(&loop, proxyPort, "roundrobin", "PoolProxy");
    server.AddBackend("127.0.0.1", backendPort, 1);
    server.Start();

    std::thread client([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // 1st request (client closes after response).
        {
            int fd = connectTo(proxyPort);
            if (fd < 0) {
                std::cerr << "Failed to connect to proxy\n";
                loop.QueueInLoop([&]() { loop.Quit(); });
                return;
            }
            sendAll(fd, "GET /a HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");
            std::string resp;
            auto start = std::chrono::steady_clock::now();
            while (resp.find("\r\n\r\n") == std::string::npos || resp.find("OK") == std::string::npos) {
                if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() > 2000) break;
                std::string chunk = recvSome(fd, 200);
                if (!chunk.empty()) resp += chunk;
            }
            if (resp.find("200") == std::string::npos || resp.find("OK") == std::string::npos) {
                ::write(STDERR_FILENO, resp.data(), resp.size());
                ::write(STDERR_FILENO, "\n", 1);
                assert(false);
            }
            ::close(fd);
        }

        // 2nd request from a new client connection.
        {
            int fd = connectTo(proxyPort);
            if (fd < 0) {
                std::cerr << "Failed to connect to proxy\n";
                loop.QueueInLoop([&]() { loop.Quit(); });
                return;
            }
            sendAll(fd, "GET /b HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");
            std::string resp;
            auto start = std::chrono::steady_clock::now();
            while (resp.find("\r\n\r\n") == std::string::npos || resp.find("OK") == std::string::npos) {
                if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() > 2000) break;
                std::string chunk = recvSome(fd, 200);
                if (!chunk.empty()) resp += chunk;
            }
            if (resp.find("200") == std::string::npos || resp.find("OK") == std::string::npos) {
                ::write(STDERR_FILENO, resp.data(), resp.size());
                ::write(STDERR_FILENO, "\n", 1);
                assert(false);
            }
            ::close(fd);
        }

        // Wait backend to see both requests.
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
        while (std::chrono::steady_clock::now() < deadline) {
            if (reqCnt.load() >= 2) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        loop.QueueInLoop([&]() { loop.Quit(); });
    });

    loop.Loop();
    client.join();

    // Pool goal: backend should accept only 1 TCP connection for 2 proxied requests.
    assert(reqCnt.load() >= 2);
    assert(acceptCnt.load() == 1);

    stopBackend.store(true);
    backend.join();
    return 0;
}
