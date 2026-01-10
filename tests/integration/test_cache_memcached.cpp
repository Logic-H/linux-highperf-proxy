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
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
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
    if (::listen(fd, 64) != 0) {
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

static void backendServer(int lfd, std::atomic<bool>* stop, std::atomic<int>* hits) {
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
            const int cur = hits->fetch_add(1) + 1;
            const std::string body = "V" + std::to_string(cur);
            std::string resp = "HTTP/1.1 200 OK\r\n"
                               "Content-Type: text/plain\r\n"
                               "Content-Length: " +
                               std::to_string(body.size()) +
                               "\r\n"
                               "Connection: close\r\n"
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

static void memcachedServer(int lfd, std::atomic<bool>* stop) {
    std::map<std::string, std::string> kv;
    std::mutex mu;
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

            // Handle "get <key>\r\n" or "set <key> 0 <ttl> <bytes>\r\n<val>\r\n"
            size_t lineEnd = in.find("\r\n");
            if (lineEnd == std::string::npos) continue;
            std::string line = in.substr(0, lineEnd);
            if (line.rfind("get ", 0) == 0) {
                std::string key = line.substr(4);
                {
                    std::lock_guard<std::mutex> lock(mu);
                    auto it = kv.find(key);
                    if (it != kv.end()) {
                        const std::string& val = it->second;
                        std::string resp = "VALUE " + key + " 0 " + std::to_string(val.size()) + "\r\n";
                        resp += val;
                        resp += "\r\nEND\r\n";
                        sendAll(cfd, resp);
                    } else {
                        sendAll(cfd, "END\r\n");
                    }
                }
                in.erase(0, lineEnd + 2);
                break;
            }
            if (line.rfind("set ", 0) == 0) {
                // Parse fields
                // set key flags ttl bytes
                std::string key;
                int flags = 0;
                int ttl = 0;
                size_t bytes = 0;
                {
                    std::istringstream iss(line);
                    std::string cmd;
                    iss >> cmd >> key >> flags >> ttl >> bytes;
                    (void)cmd;
                    (void)flags;
                    (void)ttl;
                }
                const size_t need = lineEnd + 2 + bytes + 2;
                if (in.size() < need) continue;
                const std::string val = in.substr(lineEnd + 2, bytes);
                {
                    std::lock_guard<std::mutex> lock(mu);
                    kv[key] = val;
                }
                sendAll(cfd, "STORED\r\n");
                in.erase(0, need);
                break;
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

    int cacheListenFd = -1;
    int backendListenFd = -1;
    const auto cachePortOpt = bindEphemeralTcpPort(&cacheListenFd);
    const auto backendPortOpt = bindEphemeralTcpPort(&backendListenFd);
    const auto proxyPortOpt = reserveFreeTcpPort();
    assert(cachePortOpt.has_value());
    assert(backendPortOpt.has_value());
    assert(proxyPortOpt.has_value());
    const uint16_t cachePort = *cachePortOpt;
    const uint16_t backendPort = *backendPortOpt;
    const uint16_t proxyPort = *proxyPortOpt;

    std::atomic<bool> stopCache{false};
    std::thread cacheT([&]() { memcachedServer(cacheListenFd, &stopCache); });

    std::atomic<bool> stopBackend{false};
    std::atomic<int> backendHits{0};
    std::thread backendT([&]() { backendServer(backendListenFd, &stopBackend, &backendHits); });

    EventLoop loop;
    proxy::ProxyServer server(&loop, proxyPort, "roundrobin", "CacheProxy");
    server.AddBackend("127.0.0.1", backendPort, 1);
    proxy::protocol::Cache::Config cfg;
    cfg.enabled = true;
    cfg.backend = "memcached";
    cfg.host = "127.0.0.1";
    cfg.port = cachePort;
    cfg.ttlSec = 60;
    cfg.timeoutMs = 20;
    cfg.maxValueBytes = 256 * 1024;
    server.ConfigureCache(cfg);
    server.Start();

    std::thread client([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        auto doReq = [&](std::string* bodyOut) {
            int fd = connectTo(proxyPort);
            assert(fd >= 0);
            const std::string req = "GET /cache HTTP/1.1\r\n"
                                    "Host: 127.0.0.1\r\n"
                                    "Accept-Encoding: identity\r\n"
                                    "Connection: close\r\n"
                                    "\r\n";
            sendAll(fd, req);
            const std::string raw = recvAllWithTimeout(fd, 2000);
            ::close(fd);
            const size_t hdrEnd = raw.find("\r\n\r\n");
            assert(hdrEnd != std::string::npos);
            *bodyOut = raw.substr(hdrEnd + 4);
        };

        std::string b1, b2;
        doReq(&b1);
        doReq(&b2);
        assert(b1 == b2);

        loop.QueueInLoop([&]() { loop.Quit(); });
    });

    loop.Loop();
    client.join();

    stopBackend.store(true);
    stopCache.store(true);
    backendT.join();
    cacheT.join();

    assert(backendHits.load() == 1);
    return 0;
}
