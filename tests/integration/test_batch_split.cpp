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
#include <cstdio>
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

static size_t parseContentLength(const std::string& hdrs) {
    size_t pos = 0;
    while (true) {
        size_t lineEnd = hdrs.find("\r\n", pos);
        std::string line;
        if (lineEnd == std::string::npos) {
            line = hdrs.substr(pos);
        } else {
            line = hdrs.substr(pos, lineEnd - pos);
        }
        const std::string needle = "Content-Length:";
        if (line.rfind(needle, 0) == 0) {
            std::string v = line.substr(needle.size());
            while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
            try {
                return static_cast<size_t>(std::stoull(v));
            } catch (...) {
                return 0;
            }
        }
        if (lineEnd == std::string::npos) break;
        pos = lineEnd + 2;
    }
    return 0;
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
            const size_t clen = parseContentLength(in.substr(0, hdrEnd));
            if (in.size() < hdrEnd + 4 + clen) continue;
            std::string body = in.substr(hdrEnd + 4, clen);

            // Ensure proxy split the batch: backend must never see a JSON array.
            std::string bodyTrim = body;
            while (!bodyTrim.empty() && (bodyTrim.front() == ' ' || bodyTrim.front() == '\n' || bodyTrim.front() == '\r' || bodyTrim.front() == '\t'))
                bodyTrim.erase(bodyTrim.begin());
            if (!bodyTrim.empty() && bodyTrim.front() == '[') {
                std::string resp = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                sendAll(cfd, resp);
                break;
            }

            std::string out = "{\"in\":" + bodyTrim + "}";
            std::string resp = "HTTP/1.1 200 OK\r\n"
                               "Content-Type: application/json\r\n"
                               "Content-Length: " +
                               std::to_string(out.size()) +
                               "\r\n"
                               "Connection: close\r\n"
                               "\r\n" +
                               out;
            sendAll(cfd, resp);
            break;
        }
        ::shutdown(cfd, SHUT_RDWR);
        ::close(cfd);
    }
    ::close(lfd);
}

static std::string httpRequest(uint16_t port, const std::string& req) {
    int fd = connectTo(port);
    assert(fd >= 0);
    sendAll(fd, req);
    std::string resp = recvAllWithTimeout(fd, 2000);
    ::close(fd);
    return resp;
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
    std::thread bt([&]() { backendServer(bfd, &stop); });

    EventLoop loop;
    proxy::ProxyServer server(&loop, proxyPort, "roundrobin", "BatchSplitProxy");
    server.AddBackend("127.0.0.1", bport, 1);
    server.Start();

    std::thread client([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        const std::string body = "[{\"a\":1},{\"a\":2}]";
        std::string req = "POST /infer HTTP/1.1\r\n"
                          "Host: test\r\n"
                          "Content-Type: application/json\r\n"
                          "X-Batch-Split: 1\r\n"
                          "Content-Length: " +
                          std::to_string(body.size()) +
                          "\r\n"
                          "Connection: close\r\n"
                          "\r\n" +
                          body;

        std::string resp = httpRequest(proxyPort, req);
        if (resp.find("200 OK") == std::string::npos) {
            fprintf(stderr, "bad response (no 200 OK):\n%s\n", resp.c_str());
            assert(false);
        }
        if (resp.find("\"status\":200") == std::string::npos) {
            fprintf(stderr, "bad response (no status=200):\n%s\n", resp.c_str());
            assert(false);
        }
        // The proxy wraps backend body as an escaped JSON string.
        if (resp.find("a\\\":1") == std::string::npos || resp.find("a\\\":2") == std::string::npos) {
            fprintf(stderr, "bad response (missing items):\n%s\n", resp.c_str());
            assert(false);
        }

        loop.QueueInLoop([&]() { loop.Quit(); });
    });

    loop.Loop();
    client.join();
    stop.store(true);
    bt.join();
    return 0;
}
