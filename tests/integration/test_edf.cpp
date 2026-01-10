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
    proxy::ProxyServer server(&loop, proxyPort, "roundrobin", "EdfProxy");
    server.AddBackend("127.0.0.1", bport, 1);

    proxy::ProxyServer::PriorityConfig sc;
    sc.enabled = true;
    sc.mode = "edf";
    sc.maxInflight = 1;
    sc.deadlineHeader = "X-Deadline-Ms";
    sc.defaultDeadlineMs = 60000;
    server.ConfigurePriorityScheduling(sc);
    server.Start();

    std::thread client([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(120));

        auto openAndSend = [&](const std::string& path, int deadlineMs) -> int {
            int fd = connectTo(proxyPort);
            assert(fd >= 0);
            std::string req = "GET " + path + " HTTP/1.1\r\nHost: test\r\nConnection: close\r\n";
            if (deadlineMs >= 0) req += "X-Deadline-Ms: " + std::to_string(deadlineMs) + "\r\n";
            req += "\r\n";
            sendAll(fd, req);
            return fd;
        };

        int fdHold = openAndSend("/hold", 10000);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        // Enqueue in reverse deadline order: d3(3000) first, then d1(1000), then d2(2000).
        int fdD3 = openAndSend("/d3", 3000);
        int fdD1 = openAndSend("/d1", 1000);
        int fdD2 = openAndSend("/d2", 2000);

        auto readResp = [&](int fd, const char* tag) {
            std::string resp = readAllWithTimeout(fd, 3000);
            ::close(fd);
            if (resp.find("200 OK") == std::string::npos) {
                std::string head = resp.substr(0, resp.find("\r\n\r\n") == std::string::npos ? 200 : resp.find("\r\n\r\n"));
                ::fprintf(stderr, "BAD RESP tag=%s head=%s\n", tag, head.c_str());
                assert(false);
            }
            return resp;
        };

        std::string rD1 = readResp(fdD1, "d1");
        std::string rD2 = readResp(fdD2, "d2");
        std::string rD3 = readResp(fdD3, "d3");
        std::string rHold = readResp(fdHold, "hold");
        (void)rHold;

        std::unordered_map<std::string, int> m;
        m["/d1"] = extractSeq(rD1);
        m["/d2"] = extractSeq(rD2);
        m["/d3"] = extractSeq(rD3);
        assert(m["/d1"] > 0 && m["/d2"] > 0 && m["/d3"] > 0);

        // EDF should serve earliest deadline first: d1 then d2 then d3.
        assert(m["/d1"] < m["/d2"]);
        assert(m["/d2"] < m["/d3"]);

        loop.QueueInLoop([&]() { loop.Quit(); });
    });

    loop.Loop();
    client.join();
    stop.store(true);
    bt.join();
    return 0;
}
