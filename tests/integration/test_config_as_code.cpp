#include "proxy/ProxyServer.h"
#include "proxy/common/Config.h"
#include "proxy/common/Logger.h"
#include "proxy/network/EventLoop.h"

#include <arpa/inet.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

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

static std::optional<uint16_t> reserveFreeTcpPort() {
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
    if (::listen(fd, 1) != 0) {
        ::close(fd);
        return std::nullopt;
    }
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        ::close(fd);
        return std::nullopt;
    }
    const uint16_t port = ntohs(addr.sin_port);
    ::close(fd);
    if (port == 0) return std::nullopt;
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

static std::string tmpPath(const std::string& suffix) {
    return std::string("/tmp/proxy_cfg_code_") + std::to_string(::getpid()) + "_" + suffix;
}

}  // namespace

int main() {
    ::signal(SIGPIPE, SIG_IGN);
    proxy::common::Logger::Instance().SetLevel(proxy::common::LogLevel::ERROR);

    // Create and load a temp config file so Save() has a target.
    const std::string cfgPath = tmpPath("proxy.conf");
    {
        std::ofstream f(cfgPath);
        f << "[global]\n";
        f << "log_level = INFO\n";
        f << "listen_port = 0\n\n";
        f << "[rate_limit]\n";
        f << "qps = 0\n";
    }
    assert(proxy::common::Config::Instance().Load(cfgPath));

    const auto proxyPortOpt = reserveFreeTcpPort();
    assert(proxyPortOpt.has_value());
    const uint16_t proxyPort = *proxyPortOpt;

    EventLoop loop;
    proxy::ProxyServer server(&loop, proxyPort, "roundrobin", "CfgCodeProxy");
    server.Start();

    std::thread client([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // GET INI.
        {
            int fd = connectTo(proxyPort);
            assert(fd >= 0);
            sendAll(fd, "GET /admin/config?format=ini HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
            const std::string resp = readAllWithTimeout(fd, 1500);
            ::close(fd);
            assert(resp.find("200 OK") != std::string::npos);
            assert(resp.find("[global]") != std::string::npos);
            assert(resp.find("log_level = INFO") != std::string::npos);
        }

        // POST apply new INI + save.
        {
            const std::string ini =
                "[global]\n"
                "log_level = ERROR\n"
                "\n"
                "[rate_limit]\n"
                "qps = 0\n";
            int fd = connectTo(proxyPort);
            assert(fd >= 0);
            std::string req = "POST /admin/config?format=ini&save=1 HTTP/1.1\r\nHost: test\r\n"
                              "Content-Type: text/plain\r\nContent-Length: " +
                              std::to_string(ini.size()) + "\r\nConnection: close\r\n\r\n" + ini;
            sendAll(fd, req);
            const std::string resp = readAllWithTimeout(fd, 1500);
            ::close(fd);
            assert(resp.find("\"ok\":true") != std::string::npos);
            assert(resp.find("\"saved\":true") != std::string::npos);
        }

        // File persisted.
        {
            std::ifstream f(cfgPath);
            std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            assert(all.find("log_level = ERROR") != std::string::npos);
        }

        loop.QueueInLoop([&]() { loop.Quit(); });
    });

    loop.Loop();
    client.join();
    ::unlink(cfgPath.c_str());
    return 0;
}

