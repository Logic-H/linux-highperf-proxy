#include "proxy/ProxyServer.h"
#include "proxy/common/Config.h"
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
#include <fstream>
#include <optional>
#include <string>
#include <thread>

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

static void backendHttpServer(int lfd, std::atomic<bool>* stop) {
    while (!stop->load()) {
        if (!pollReadable(lfd, 200)) continue;
        int cfd = ::accept(lfd, nullptr, nullptr);
        if (cfd < 0) continue;
        (void)readAllWithTimeout(cfd, 300);
        const char* resp = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK";
        ::send(cfd, resp, std::strlen(resp), 0);
        ::shutdown(cfd, SHUT_RDWR);
        ::close(cfd);
    }
    ::close(lfd);
}

static bool fileContains(const std::string& path, const std::string& needle) {
    std::ifstream f(path);
    if (!f.is_open()) return false;
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return s.find(needle) != std::string::npos;
}

} // namespace

int main() {
    ::signal(SIGPIPE, SIG_IGN);
    proxy::common::Logger::Instance().SetLevel(proxy::common::LogLevel::ERROR);

    const std::string cfgPath = "/tmp/proxy_diag_" + std::to_string(::getpid()) + "_proxy.conf";
    const std::string auditPath = "/tmp/proxy_diag_" + std::to_string(::getpid()) + "_audit.log";
    {
        std::ofstream f(cfgPath);
        f << "[global]\n";
        f << "log_level = ERROR\n";
    }
    proxy::common::Config::Instance().Load(cfgPath);

    int bfd = -1;
    const auto bportOpt = bindEphemeralTcpPort(&bfd);
    const auto proxyPortOpt = reserveFreeTcpPort();
    assert(bportOpt.has_value());
    assert(proxyPortOpt.has_value());
    const uint16_t bport = *bportOpt;
    const uint16_t proxyPort = *proxyPortOpt;

    std::atomic<bool> stop{false};
    std::thread bt([&]() { backendHttpServer(bfd, &stop); });

    EventLoop loop;
    proxy::ProxyServer server(&loop, proxyPort, "roundrobin", "DiagProxy");
    server.AddBackend("127.0.0.1", bport, 1);
    server.EnableAuditLog(auditPath);
    server.Start();

    std::thread client([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // 1) Hit /stats to generate an audit line.
        {
            int fd = connectTo(proxyPort);
            assert(fd >= 0);
            const std::string req = "GET /stats HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n";
            sendAll(fd, req);
            const std::string resp = readAllWithTimeout(fd, 1500);
            ::close(fd);
            assert(resp.find("200 OK") != std::string::npos);
        }
        // Allow flush.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        assert(fileContains(auditPath, "path=/stats"));

        // 2) Tail audit logs via /admin/logs.
        {
            int fd = connectTo(proxyPort);
            assert(fd >= 0);
            const std::string req = "GET /admin/logs?type=audit&lines=50 HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n";
            sendAll(fd, req);
            const std::string resp = readAllWithTimeout(fd, 1500);
            ::close(fd);
            assert(resp.find("200 OK") != std::string::npos);
            assert(resp.find("path=/stats") != std::string::npos);
        }

        // 3) Combined diagnose JSON.
        {
            int fd = connectTo(proxyPort);
            assert(fd >= 0);
            const std::string req = "GET /admin/diagnose HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n";
            sendAll(fd, req);
            const std::string resp = readAllWithTimeout(fd, 1500);
            ::close(fd);
            assert(resp.find("200 OK") != std::string::npos);
            assert(resp.find("\"ok\":1") != std::string::npos);
            assert(resp.find(cfgPath) != std::string::npos);
            assert(resp.find(auditPath) != std::string::npos);
            assert(resp.find("\"stats\"") != std::string::npos);
        }

        // 4) Diagnostics HTML UI.
        {
            int fd = connectTo(proxyPort);
            assert(fd >= 0);
            const std::string req = "GET /diagnostics HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n";
            sendAll(fd, req);
            const std::string resp = readAllWithTimeout(fd, 1500);
            ::close(fd);
            assert(resp.find("200 OK") != std::string::npos);
            assert(resp.find("<title>Proxy Diagnostics</title>") != std::string::npos);
            assert(resp.find("/admin/diagnose") != std::string::npos);
            assert(resp.find("/admin/logs") != std::string::npos);
        }

        loop.QueueInLoop([&]() { loop.Quit(); });
    });

    loop.Loop();
    client.join();
    stop.store(true);
    bt.join();
    return 0;
}

