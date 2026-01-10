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
#include <vector>

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

static std::string recvHttpResponse(int fd, int timeoutMs = 2000) {
    std::string in;
    auto start = std::chrono::steady_clock::now();
    while (true) {
        if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() > timeoutMs) break;
        if (!pollReadable(fd, 200)) continue;
        char buf[4096];
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        in.append(buf, buf + n);
        const size_t hdrEnd = in.find("\r\n\r\n");
        if (hdrEnd == std::string::npos) continue;
        // best-effort Content-Length
        size_t clPos = in.find("Content-Length:");
        if (clPos == std::string::npos) clPos = in.find("Content-length:");
        size_t need = 0;
        if (clPos != std::string::npos) {
            size_t lineEnd = in.find("\r\n", clPos);
            if (lineEnd != std::string::npos) {
                std::string v = in.substr(clPos + strlen("Content-Length:"), lineEnd - (clPos + strlen("Content-Length:")));
                while (!v.empty() && (v[0] == ' ' || v[0] == '\t')) v.erase(v.begin());
                need = static_cast<size_t>(std::atoi(v.c_str()));
            }
        }
        const size_t bodyHave = in.size() - (hdrEnd + 4);
        if (need > 0 && bodyHave >= need) break;
        if (need == 0 && bodyHave > 0) break;
    }
    return in;
}

static void sendJsonPost(uint16_t port, const std::string& body, const std::string& model, std::string* outResp) {
    int fd = connectTo(port);
    assert(fd >= 0);
    std::string req = "POST /infer HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n"
                      "Content-Type: application/json\r\n";
    if (!model.empty()) req += "X-Model: " + model + "\r\n";
    req += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    req += body;
    sendAll(fd, req);
    *outResp = recvHttpResponse(fd);
    ::close(fd);
}

static void batchBackend(int lfd, std::atomic<int>* acceptCnt, std::atomic<int>* reqCnt, std::atomic<bool>* stop) {
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
            in.append(buf, buf + n);
            if (in.size() > 512 * 1024) break;

            while (true) {
                size_t hdrEnd = in.find("\r\n\r\n");
                if (hdrEnd == std::string::npos) break;
                size_t clPos = in.find("Content-Length:");
                if (clPos == std::string::npos) clPos = in.find("Content-length:");
                size_t len = 0;
                if (clPos != std::string::npos) {
                    size_t lineEnd = in.find("\r\n", clPos);
                    if (lineEnd != std::string::npos) {
                        std::string v = in.substr(clPos + strlen("Content-Length:"), lineEnd - (clPos + strlen("Content-Length:")));
                        while (!v.empty() && (v[0] == ' ' || v[0] == '\t')) v.erase(v.begin());
                        len = static_cast<size_t>(std::atoi(v.c_str()));
                    }
                }
                if (in.size() < hdrEnd + 4 + len) break;
                const std::string body = in.substr(hdrEnd + 4, len);
                reqCnt->fetch_add(1);

                // Expect batched body: [ {...}, {...} ]
                std::string outBody = body;
                const std::string resp = "HTTP/1.1 200 OK\r\n"
                                         "Content-Type: application/json\r\n"
                                         "Content-Length: " +
                                         std::to_string(outBody.size()) +
                                         "\r\n"
                                         "Connection: keep-alive\r\n"
                                         "\r\n" +
                                         outBody;
                sendAll(cfd, resp);
                in.erase(0, hdrEnd + 4 + len);
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
    proxy::common::Logger::Instance().SetLevel(proxy::common::LogLevel::ERROR);

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

    std::thread backend([&]() { batchBackend(backendListenFd, &acceptCnt, &reqCnt, &stopBackend); });

    EventLoop loop;
    proxy::ProxyServer server(&loop, proxyPort, "roundrobin", "BatchProxy");
    server.AddBackend("127.0.0.1", backendPort, 1);
    {
        proxy::protocol::HttpBatcher::Config cfg;
        cfg.enabled = true;
        cfg.windowMs = 200;
        cfg.maxBatchSize = 8;
        cfg.maxBatchBytes = 256 * 1024;
        cfg.maxResponseBytes = 1024 * 1024;
        cfg.paths = {"/infer"};
        cfg.requireHeader = false;
        server.ConfigureHttpBatching(cfg);
    }
    server.Start();

    std::thread client([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        std::string r1, r2;
        std::thread t1([&]() { sendJsonPost(proxyPort, "{\"req\":1}", "m1", &r1); });
        std::thread t2([&]() { sendJsonPost(proxyPort, "{\"req\":2}", "m1", &r2); });
        t1.join();
        t2.join();

        if (r1.find("200") == std::string::npos || r1.find("{\"req\":1}") == std::string::npos) {
            ::write(STDERR_FILENO, r1.data(), r1.size());
            ::write(STDERR_FILENO, "\n", 1);
            assert(false);
        }
        if (r2.find("200") == std::string::npos || r2.find("{\"req\":2}") == std::string::npos) {
            ::write(STDERR_FILENO, r2.data(), r2.size());
            ::write(STDERR_FILENO, "\n", 1);
            assert(false);
        }

        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
        while (std::chrono::steady_clock::now() < deadline) {
            if (reqCnt.load() >= 1) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        loop.QueueInLoop([&]() { loop.Quit(); });
    });

    loop.Loop();
    client.join();

    // Batching goal: backend should see exactly 1 HTTP request for 2 client requests.
    assert(reqCnt.load() == 1);
    assert(acceptCnt.load() == 1);

    stopBackend.store(true);
    backend.join();
    return 0;
}
