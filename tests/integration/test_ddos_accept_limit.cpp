#include "proxy/ProxyServer.h"
#include "proxy/common/Logger.h"
#include "proxy/network/EventLoop.h"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>

using proxy::network::EventLoop;

namespace {

static uint16_t pickFreePort() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    assert(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);

    socklen_t len = sizeof(addr);
    assert(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    uint16_t port = ntohs(addr.sin_port);
    ::close(fd);
    assert(port != 0);
    return port;
}

static int connectTo(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    assert(::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1);
    int ret = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    assert(ret == 0);
    return fd;
}

static bool pollReadable(int fd, int timeoutMs) {
    pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN | POLLHUP | POLLERR;
    int ret = ::poll(&pfd, 1, timeoutMs);
    return ret == 1;
}

static std::string recvUntilClose(int fd, int timeoutMs = 2000) {
    std::string out;
    while (pollReadable(fd, timeoutMs)) {
        char buf[4096];
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n > 0) out.append(buf, buf + n);
        else break;
    }
    return out;
}

} // namespace

int main() {
    proxy::common::Logger::Instance().SetLevel(proxy::common::LogLevel::ERROR);

    const uint16_t port = pickFreePort();
    EventLoop loop;
    proxy::ProxyServer server(&loop, port, "roundrobin", "DdosAcceptTest");
    server.SetAcceptRateLimit(20.0, 20.0);
    server.SetPerIpAcceptRateLimit(10.0, 10.0, 60.0, 10000);
    server.Start();

    std::thread client([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // Burst a lot of connects quickly; some should be dropped at accept.
        for (int i = 0; i < 200; ++i) {
            int fd = connectTo(port);
            ::close(fd);
        }

        // Let token buckets refill so /stats request can pass.
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        int fd = connectTo(port);
        const char* req = "GET /stats HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n";
        ::send(fd, req, std::strlen(req), 0);
        std::string resp = recvUntilClose(fd, 2000);
        ::close(fd);

        // Ensure the ddos counter exists and is non-zero.
        assert(resp.find("\"ddos_drops\": ") != std::string::npos);
        const auto pos = resp.find("\"ddos_drops\": ");
        assert(pos != std::string::npos);
        size_t p = pos + std::strlen("\"ddos_drops\": ");
        while (p < resp.size() && (resp[p] == ' ')) ++p;
        long long v = 0;
        while (p < resp.size() && resp[p] >= '0' && resp[p] <= '9') {
            v = v * 10 + (resp[p] - '0');
            ++p;
        }
        assert(v > 0);

        loop.QueueInLoop([&]() { loop.Quit(); });
    });

    loop.Loop();
    client.join();
    return 0;
}

