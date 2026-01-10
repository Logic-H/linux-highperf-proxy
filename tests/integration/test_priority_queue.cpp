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
#include <condition_variable>
#include <cstring>
#include <mutex>
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
    if (::listen(fd, 32) != 0) {
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
 
struct BackendSync {
    std::mutex mu;
    std::condition_variable cv;
    int seen{0};
};
 
static std::string readRequestHead(int fd, std::atomic<bool>* stop) {
    std::string in;
    while (!stop->load()) {
        if (!pollReadable(fd, 200)) continue;
        char buf[4096];
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        in.append(buf, buf + n);
        if (in.find("\r\n\r\n") != std::string::npos) break;
        if (in.size() > 8192) break;
    }
    return in;
}
 
static std::string parsePathFromRequestLine(const std::string& head) {
    // "GET /path?x=1 HTTP/1.1"
    const size_t eol = head.find("\r\n");
    const std::string first = (eol == std::string::npos) ? head : head.substr(0, eol);
    const size_t sp1 = first.find(' ');
    if (sp1 == std::string::npos) return "/";
    const size_t sp2 = first.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) return "/";
    return first.substr(sp1 + 1, sp2 - (sp1 + 1));
}
 
static void backendSlowHttpServer(int lfd, std::atomic<bool>* stop, BackendSync* sync, int delayMs) {
    while (!stop->load()) {
        if (!pollReadable(lfd, 200)) continue;
        int cfd = ::accept(lfd, nullptr, nullptr);
        if (cfd < 0) continue;
 
        while (!stop->load()) {
            const std::string head = readRequestHead(cfd, stop);
            if (head.empty()) break;
            const std::string path = parsePathFromRequestLine(head);
 
            {
                std::lock_guard<std::mutex> lock(sync->mu);
                sync->seen++;
                sync->cv.notify_all();
            }
 
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
 
            std::string body = "OK " + path;
            std::string resp = "HTTP/1.1 200 OK\r\n"
                               "Content-Type: text/plain\r\n"
                               "Connection: Keep-Alive\r\n"
                               "Content-Length: " +
                               std::to_string(body.size()) + "\r\n"
                                                            "\r\n" +
                               body;
            sendAll(cfd, resp);
        }
        ::shutdown(cfd, SHUT_RDWR);
        ::close(cfd);
    }
    ::close(lfd);
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
 
struct ClientResult {
    std::string name;
    std::string resp;
    double sendAtMs{0};
    double doneAtMs{0};
};
 
static ClientResult doClient(uint16_t proxyPort,
                             const std::string& path,
                             const std::string& prioHdr,
                             std::chrono::steady_clock::time_point t0) {
    ClientResult r;
    r.name = path;
    int fd = connectTo(proxyPort);
    assert(fd >= 0);
    std::string req = "GET " + path + " HTTP/1.1\r\n"
                      "Host: test\r\n"
                      "Accept-Encoding: identity\r\n"
                      "Connection: close\r\n";
    if (!prioHdr.empty()) req += "X-Priority: " + prioHdr + "\r\n";
    req += "\r\n";
    const auto start = std::chrono::steady_clock::now();
    r.sendAtMs = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(start - t0).count();
    sendAll(fd, req);
    r.resp = readAllWithTimeout(fd, 3000);
    ::close(fd);
    r.doneAtMs =
        std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(std::chrono::steady_clock::now() - t0).count();
    return r;
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
    BackendSync sync;
    std::thread bt([&]() { backendSlowHttpServer(bfd, &stop, &sync, 400); });
 
    EventLoop loop;
    proxy::ProxyServer server(&loop, proxyPort, "roundrobin", "PrioProxy");
    server.AddBackend("127.0.0.1", bport, 1);
 
    proxy::ProxyServer::PriorityConfig pc;
    pc.enabled = true;
    pc.maxInflight = 1;     // enforce queueing
    pc.highThreshold = 8;   // >=8 treated as high priority
    pc.lowDelayMs = 0;
    pc.priorityHeader = "X-Priority";
    pc.priorityQuery = "priority";
    server.ConfigurePriorityScheduling(pc);
 
    server.Start();
 
    ClientResult low1, low2, high;
    std::thread client([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const auto t0 = std::chrono::steady_clock::now();
 
        // Start low1 and wait until backend has seen it (inflight occupied).
        std::thread t1([&]() { low1 = doClient(proxyPort, "/low1", "1", t0); });
        {
            std::unique_lock<std::mutex> lock(sync.mu);
            sync.cv.wait_for(lock, std::chrono::milliseconds(1200), [&]() { return sync.seen >= 1; });
        }
 
        std::thread t2([&]() { low2 = doClient(proxyPort, "/low2", "1", t0); });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::thread t3([&]() { high = doClient(proxyPort, "/high", "9", t0); });
        t1.join();
        t2.join();
        t3.join();
 
        loop.QueueInLoop([&]() { loop.Quit(); });
    });
 
    loop.Loop();
    client.join();
 
    stop.store(true);
    bt.join();
 
    // Basic sanity.
    assert(low1.resp.find("200 OK") != std::string::npos);
    assert(low2.resp.find("200 OK") != std::string::npos);
    assert(high.resp.find("200 OK") != std::string::npos);
    assert(low1.resp.find("OK /low1") != std::string::npos);
    assert(low2.resp.find("OK /low2") != std::string::npos);
    assert(high.resp.find("OK /high") != std::string::npos);

    // Under contention (maxInflight=1), high-priority request should complete before queued low-priority one.
    assert(low2.sendAtMs <= high.sendAtMs);
    assert(high.doneAtMs + 150.0 < low2.doneAtMs);
    return 0;
}
