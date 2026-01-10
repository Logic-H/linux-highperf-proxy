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

static std::optional<uint16_t> bindEphemeralUdpPort(int* fdOut) {
    int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
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
    *fdOut = fd;
    return port;
}

static void backendServer(int lfd, std::atomic<bool>* stop) {
    while (!stop->load()) {
        if (!pollReadable(lfd, 200)) continue;
        int cfd = ::accept(lfd, nullptr, nullptr);
        if (cfd < 0) continue;
        std::string in;
        while (!stop->load()) {
            if (!pollReadable(cfd, 200)) continue;
            char buf[4096];
            ssize_t n = ::recv(cfd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            in.append(buf, buf + n);
            size_t hdrEnd = in.find("\r\n\r\n");
            if (hdrEnd == std::string::npos) continue;
            // Respond immediately; body parsing not required for this test.
            const std::string respBody = "OK";
            const std::string resp = "HTTP/1.1 200 OK\r\n"
                                     "Content-Type: text/plain\r\n"
                                     "Content-Length: " +
                                     std::to_string(respBody.size()) +
                                     "\r\n"
                                     "Connection: close\r\n"
                                     "\r\n" +
                                     respBody;
            sendAll(cfd, resp);
            break;
        }
        ::shutdown(cfd, SHUT_RDWR);
        ::close(cfd);
    }
    ::close(lfd);
}

static std::string recvAllWithTimeout(int fd, int timeoutMs) {
    auto start = std::chrono::steady_clock::now();
    std::string out;
    while (true) {
        if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() > timeoutMs) break;
        if (!pollReadable(fd, 200)) continue;
        char buf[8192];
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        out.append(buf, buf + n);
        if (out.find("\r\n\r\n") != std::string::npos) break;
    }
    return out;
}

} // namespace

int main() {
    ::signal(SIGPIPE, SIG_IGN);
    proxy::common::Logger::Instance().SetLevel(proxy::common::LogLevel::ERROR);

    int mirrorFd = -1;
    const auto mirrorPortOpt = bindEphemeralUdpPort(&mirrorFd);
    assert(mirrorPortOpt.has_value());
    const uint16_t mirrorPort = *mirrorPortOpt;

    int backendListenFd = -1;
    const auto backendPortOpt = bindEphemeralTcpPort(&backendListenFd);
    const auto proxyPortOpt = reserveFreeTcpPort();
    assert(backendPortOpt.has_value());
    assert(proxyPortOpt.has_value());
    const uint16_t backendPort = *backendPortOpt;
    const uint16_t proxyPort = *proxyPortOpt;

    std::atomic<bool> stopBackend{false};
    std::thread backend([&]() { backendServer(backendListenFd, &stopBackend); });

    std::atomic<bool> gotMirror{false};
    std::string mirrorMsg;
    std::thread receiver([&]() {
        auto start = std::chrono::steady_clock::now();
        while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() < 3000) {
            if (!pollReadable(mirrorFd, 200)) continue;
            char buf[4096];
            ssize_t n = ::recvfrom(mirrorFd, buf, sizeof(buf), 0, nullptr, nullptr);
            if (n <= 0) continue;
            mirrorMsg.assign(buf, buf + n);
            if (mirrorMsg.find("\"event\":\"request\"") != std::string::npos &&
                mirrorMsg.find("\"path\":\"/m\"") != std::string::npos &&
                mirrorMsg.find("\"req_body\":\"PING\"") != std::string::npos) {
                gotMirror.store(true);
                break;
            }
        }
    });

    EventLoop loop;
    proxy::ProxyServer server(&loop, proxyPort, "roundrobin", "MirrorProxy");
    server.AddBackend("127.0.0.1", backendPort, 1);
    proxy::protocol::TrafficMirror::Config cfg;
    cfg.enabled = true;
    cfg.udpHost = "127.0.0.1";
    cfg.udpPort = mirrorPort;
    cfg.sampleRate = 1.0;
    cfg.maxBytes = 4096;
    cfg.maxBodyBytes = 64;
    cfg.includeReqBody = true;
    cfg.includeRespBody = false;
    server.ConfigureTrafficMirror(cfg);
    server.Start();

    std::thread client([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        int fd = connectTo(proxyPort);
        assert(fd >= 0);
        const std::string body = "PING";
        std::string req = "POST /m HTTP/1.1\r\n"
                          "Host: 127.0.0.1\r\n"
                          "Accept-Encoding: identity\r\n"
                          "Content-Length: " +
                          std::to_string(body.size()) +
                          "\r\n"
                          "Connection: close\r\n"
                          "\r\n";
        req += body;
        sendAll(fd, req);
        const std::string raw = recvAllWithTimeout(fd, 2000);
        assert(raw.find("200 OK") != std::string::npos);
        assert(raw.find("\r\n\r\nOK") != std::string::npos);
        ::close(fd);

        loop.QueueInLoop([&]() { loop.Quit(); });
    });

    loop.Loop();
    client.join();

    receiver.join();
    assert(gotMirror.load());

    stopBackend.store(true);
    backend.join();

    ::close(mirrorFd);
    return 0;
}

