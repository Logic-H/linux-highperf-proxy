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
#include <unordered_map>

using proxy::network::EventLoop;

namespace {

static bool pollReadable(int fd, int timeoutMs) {
    pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN | POLLHUP | POLLERR;
    return ::poll(&pfd, 1, timeoutMs) == 1;
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

static std::string readAllWithTimeout(int fd, int timeoutMs) {
    const auto start = std::chrono::steady_clock::now();
    std::string out;
    while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() < timeoutMs) {
        if (!pollReadable(fd, 50)) continue;
        char buf[4096];
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        out.append(buf, buf + n);
    }
    return out;
}

static std::string parsePath(const std::string& reqLine) {
    // "GET /path HTTP/1.1"
    size_t sp1 = reqLine.find(' ');
    if (sp1 == std::string::npos) return "/";
    size_t sp2 = reqLine.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) return "/";
    return reqLine.substr(sp1 + 1, sp2 - (sp1 + 1));
}

static void backendServer(int lfd, std::atomic<bool>* stop, std::atomic<int>* seq) {
    while (!stop->load()) {
        if (!pollReadable(lfd, 200)) continue;
        int cfd = ::accept(lfd, nullptr, nullptr);
        if (cfd < 0) continue;

        std::string req = readAllWithTimeout(cfd, 800);
        std::string firstLine;
        size_t eol = req.find("\r\n");
        if (eol != std::string::npos) firstLine = req.substr(0, eol);
        const std::string path = parsePath(firstLine);

        // Make /hold slow to ensure other requests queue up behind max_inflight=1.
        if (path == "/hold") std::this_thread::sleep_for(std::chrono::milliseconds(250));
        else std::this_thread::sleep_for(std::chrono::milliseconds(10));

        const int s = seq->fetch_add(1) + 1;
        const std::string body = "path=" + path + " seq=" + std::to_string(s);
        std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: " +
                           std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        (void)::send(cfd, resp.data(), resp.size(), 0);
        ::shutdown(cfd, SHUT_RDWR);
        ::close(cfd);
    }
    ::close(lfd);
}

static int extractSeq(const std::string& resp) {
    const size_t p = resp.find("seq=");
    if (p == std::string::npos) return -1;
    size_t i = p + 4;
    size_t j = i;
    while (j < resp.size() && resp[j] >= '0' && resp[j] <= '9') ++j;
    if (j == i) return -1;
    return std::stoi(resp.substr(i, j - i));
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
    std::atomic<int> seq{0};
    std::thread bt([&]() { backendServer(bfd, &stop, &seq); });

    EventLoop loop;
    proxy::ProxyServer server(&loop, proxyPort, "roundrobin", "FairProxy");
    server.AddBackend("127.0.0.1", bport, 1);

    proxy::ProxyServer::PriorityConfig sc;
    sc.enabled = true;
    sc.mode = "fair";
    sc.maxInflight = 1;
    sc.flowHeader = "X-Flow";
    server.ConfigurePriorityScheduling(sc);
    server.Start();

    std::thread client([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(120));

        auto openAndSend = [&](const std::string& path, const std::string& flow) -> int {
            int fd = connectTo(proxyPort);
            assert(fd >= 0);
            std::string req = "GET " + path + " HTTP/1.1\r\nHost: test\r\nConnection: close\r\nX-Flow: " + flow + "\r\n\r\n";
            sendAll(fd, req);
            return fd;
        };

        // 1) occupy inflight (do not wait for response yet)
        int fdHold = openAndSend("/hold", "H");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        // 2) enqueue A then A, then B then B while /hold is still running (proxy max_inflight=1).
        int fdA2 = openAndSend("/a2", "A");
        int fdA3 = openAndSend("/a3", "A");
        int fdB1 = openAndSend("/b1", "B");
        int fdB2 = openAndSend("/b2", "B");

        auto readResp = [&](int fd) {
            std::string resp = readAllWithTimeout(fd, 3000);
            ::close(fd);
            assert(resp.find("200 OK") != std::string::npos);
            return resp;
        };

        std::string rA2 = readResp(fdA2);
        std::string rA3 = readResp(fdA3);
        std::string rB1 = readResp(fdB1);
        std::string rB2 = readResp(fdB2);
        std::string rHold = readResp(fdHold);
        (void)rHold;

        std::unordered_map<std::string, int> m;
        m["/a2"] = extractSeq(rA2);
        m["/a3"] = extractSeq(rA3);
        m["/b1"] = extractSeq(rB1);
        m["/b2"] = extractSeq(rB2);
        assert(m["/a2"] > 0 && m["/b1"] > 0 && m["/a3"] > 0 && m["/b2"] > 0);

        // Fair queuing should alternate once both flows are present:
        // Expected: a2 then b1 then a3 then b2 (different from FIFO).
        assert(m["/a2"] < m["/b1"]);
        assert(m["/b1"] < m["/a3"]);
        assert(m["/a3"] < m["/b2"]);

        loop.QueueInLoop([&]() { loop.Quit(); });
    });

    loop.Loop();
    client.join();
    stop.store(true);
    bt.join();
    return 0;
}
