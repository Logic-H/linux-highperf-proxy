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

static std::optional<uint16_t> bindEphemeralTcpPort(int* listenFdOut) {
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

static std::optional<uint16_t> reserveFreeTcpPort() {
    int fd = -1;
    auto port = bindEphemeralTcpPort(&fd);
    if (fd >= 0) ::close(fd);
    return port;
}

static void backendChunkedServer(int lfd, std::atomic<bool>* stop) {
    while (!stop->load()) {
        if (!pollReadable(lfd, 200)) continue;
        int cfd = ::accept(lfd, nullptr, nullptr);
        if (cfd < 0) continue;

        // Read request headers (best-effort).
        std::string in;
        while (!stop->load()) {
            if (!pollReadable(cfd, 200)) continue;
            char buf[4096];
            ssize_t n = ::recv(cfd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            in.append(buf, buf + n);
            if (in.find("\r\n\r\n") != std::string::npos) break;
        }

        // Send a chunked response in two parts with a delay to simulate streaming inference.
        std::string hdr = "HTTP/1.1 200 OK\r\n"
                          "Content-Type: text/plain\r\n"
                          "Transfer-Encoding: chunked\r\n"
                          "Connection: close\r\n"
                          "\r\n";
        sendAll(cfd, hdr);
        sendAll(cfd, "1\r\nA\r\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        sendAll(cfd, "1\r\nB\r\n");
        sendAll(cfd, "0\r\n\r\n");

        ::shutdown(cfd, SHUT_RDWR);
        ::close(cfd);
    }
    ::close(lfd);
}

} // namespace

int main() {
    ::signal(SIGPIPE, SIG_IGN);
    proxy::common::Logger::Instance().SetLevel(proxy::common::LogLevel::ERROR);

    int bfd = -1;
    const auto bportOpt = bindEphemeralTcpPort(&bfd);
    const auto proxyPortOpt = reserveFreeTcpPort();
    assert(bportOpt.has_value());
    assert(proxyPortOpt.has_value());
    const uint16_t bport = *bportOpt;
    const uint16_t proxyPort = *proxyPortOpt;

    std::atomic<bool> stop{false};
    std::thread bt([&]() { backendChunkedServer(bfd, &stop); });

    EventLoop loop;
    proxy::ProxyServer server(&loop, proxyPort, "roundrobin", "StreamProxy");
    server.AddBackend("127.0.0.1", bport, 1);
    server.Start();

    std::thread client([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        int fd = connectTo(proxyPort);
        assert(fd >= 0);
        const std::string req = "POST /infer HTTP/1.1\r\n"
                                "Host: test\r\n"
                                "Accept-Encoding: gzip\r\n"
                                "X-Stream: 1\r\n"
                                "Content-Type: application/json\r\n"
                                "Content-Length: 2\r\n"
                                "Connection: close\r\n"
                                "\r\n"
                                "{}";
        sendAll(fd, req);

        const auto start = std::chrono::steady_clock::now();
        std::string got;

        // Wait for first chunk "A" quickly (streaming), not after full response conversion.
        while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() < 800) {
            if (!pollReadable(fd, 50)) continue;
            char buf[4096];
            ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            got.append(buf, buf + n);
            if (got.find("\r\nA\r\n") != std::string::npos || got.find("A") != std::string::npos) break;
        }
        const auto firstMs =
            std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(std::chrono::steady_clock::now() - start).count();
        assert(firstMs < 500.0);

        // Drain remaining (wait up to 2s for the delayed chunk).
        const auto drainStart = std::chrono::steady_clock::now();
        while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - drainStart).count() < 2000) {
            if (!pollReadable(fd, 200)) continue;
            char buf[4096];
            ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            got.append(buf, buf + n);
        }
        ::close(fd);

        // Must contain both chunks.
        assert(got.find("A") != std::string::npos);
        assert(got.find("B") != std::string::npos);

        loop.QueueInLoop([&]() { loop.Quit(); });
    });

    loop.Loop();
    client.join();
    stop.store(true);
    bt.join();
    return 0;
}
