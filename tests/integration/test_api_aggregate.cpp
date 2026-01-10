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
        const size_t hdrEnd = out.find("\r\n\r\n");
        if (hdrEnd != std::string::npos) {
            // If Content-Length exists, wait for full body.
            size_t pos = 0;
            size_t contentLen = 0;
            while (true) {
                size_t lineEnd = out.find("\r\n", pos);
                if (lineEnd == std::string::npos || lineEnd > hdrEnd) break;
                const std::string line = out.substr(pos, lineEnd - pos);
                const std::string needle = "Content-Length:";
                if (line.rfind(needle, 0) == 0) {
                    std::string v = line.substr(needle.size());
                    while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
                    try {
                        contentLen = static_cast<size_t>(std::stoull(v));
                    } catch (...) {
                        contentLen = 0;
                    }
                }
                pos = lineEnd + 2;
            }
            if (out.size() >= hdrEnd + 4 + contentLen) break;
        }
    }
    return out;
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
            const size_t lineEnd = in.find("\r\n");
            std::string line = (lineEnd == std::string::npos) ? in : in.substr(0, lineEnd);
            std::string body = "X";
            if (line.find(" /a ") != std::string::npos) body = "A";
            if (line.find(" /b ") != std::string::npos) body = "B";

            std::string resp = "HTTP/1.1 200 OK\r\n"
                               "Content-Type: text/plain\r\n"
                               "Content-Length: " +
                               std::to_string(body.size()) +
                               "\r\n"
                               "Connection: keep-alive\r\n"
                               "\r\n";
            resp += body;
            sendAll(cfd, resp);
            break;
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

    int b1fd = -1;
    int b2fd = -1;
    const auto b1portOpt = bindEphemeralTcpPort(&b1fd);
    const auto b2portOpt = bindEphemeralTcpPort(&b2fd);
    const auto proxyPortOpt = reserveFreeTcpPort();
    assert(b1portOpt.has_value());
    assert(b2portOpt.has_value());
    assert(proxyPortOpt.has_value());
    const uint16_t b1port = *b1portOpt;
    const uint16_t b2port = *b2portOpt;
    const uint16_t proxyPort = *proxyPortOpt;

    std::atomic<bool> stop1{false};
    std::atomic<bool> stop2{false};
    std::thread t1([&]() { backendServer(b1fd, &stop1); });
    std::thread t2([&]() { backendServer(b2fd, &stop2); });

    EventLoop loop;
    proxy::ProxyServer server(&loop, proxyPort, "roundrobin", "AggProxy");
    server.AddBackend("127.0.0.1", b1port, 1);
    server.AddBackend("127.0.0.1", b2port, 1);
    server.Start();

    std::thread client([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        int fd = connectTo(proxyPort);
        assert(fd >= 0);
        const std::string body = "{\"requests\":[{\"path\":\"/a\"},{\"path\":\"/b\"}]}";
        std::string req = "POST /aggregate HTTP/1.1\r\n"
                          "Host: 127.0.0.1\r\n"
                          "Accept-Encoding: identity\r\n"
                          "Content-Type: application/json\r\n"
                          "Content-Length: " +
                          std::to_string(body.size()) +
                          "\r\n"
                          "Connection: close\r\n"
                          "\r\n";
        req += body;
        sendAll(fd, req);
        const std::string raw = recvAllWithTimeout(fd, 2000);
        assert(raw.find("200 OK") != std::string::npos);
        assert(raw.find("\"body\":\"A\"") != std::string::npos);
        assert(raw.find("\"body\":\"B\"") != std::string::npos);
        ::close(fd);

        loop.QueueInLoop([&]() { loop.Quit(); });
    });

    loop.Loop();
    client.join();

    stop1.store(true);
    stop2.store(true);
    t1.join();
    t2.join();
    return 0;
}
