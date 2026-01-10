#include "proxy/ProxyServer.h"
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

static bool fileExists(const std::string& p) { return ::access(p.c_str(), R_OK) == 0; }

}  // namespace

int main() {
    ::signal(SIGPIPE, SIG_IGN);
    proxy::common::Logger::Instance().SetLevel(proxy::common::LogLevel::ERROR);

    const auto proxyPortOpt = reserveFreeTcpPort();
    assert(proxyPortOpt.has_value());
    const uint16_t proxyPort = *proxyPortOpt;

    EventLoop loop;
    proxy::ProxyServer server(&loop, proxyPort, "roundrobin", "PluginProxy");

    proxy::common::PluginManager::Config pc;
    pc.enabled = true;
    pc.paths = {"build/plugins/libproxy_example_plugin.so"};
    pc.httpPathPrefixes = {"/plugin"};
    assert(fileExists(pc.paths[0]));
    server.ConfigurePlugins(pc);
    server.Start();

    std::thread client([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        int fd = connectTo(proxyPort);
        assert(fd >= 0);
        sendAll(fd, "GET /plugin/hello HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
        const std::string resp = readAllWithTimeout(fd, 1500);
        ::close(fd);
        assert(resp.find("200") != std::string::npos);
        assert(resp.find("X-Plugin: example") != std::string::npos);
        assert(resp.find("\"plugin\":\"example\"") != std::string::npos);
        loop.QueueInLoop([&]() { loop.Quit(); });
    });

    loop.Loop();
    client.join();
    return 0;
}

