#include "proxy/network/UdpProxyServer.h"
#include "proxy/network/EventLoop.h"
#include "proxy/network/InetAddress.h"
#include "proxy/common/Logger.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

using namespace proxy::common;
using namespace proxy::network;

static int bindUdp(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    assert(fd >= 0);

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    int ret = ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    assert(ret == 0);
    return fd;
}

int main() {
    Logger::Instance().SetLevel(LogLevel::INFO);

    constexpr uint16_t backendPort = 9991;
    constexpr uint16_t proxyPort = 9992;

    std::atomic<bool> backendReady{false};
    std::atomic<bool> backendStop{false};

    // Backend UDP echo server
    std::thread backend([&]() {
        int fd = bindUdp(backendPort);
        backendReady.store(true);
        while (!backendStop.load()) {
            char buf[2048];
            sockaddr_in peer;
            socklen_t len = sizeof(peer);
            ssize_t n = ::recvfrom(fd, buf, sizeof(buf), MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&peer), &len);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }
                break;
            }
            ::sendto(fd, buf, static_cast<size_t>(n), 0, reinterpret_cast<sockaddr*>(&peer), len);
        }
        ::close(fd);
    });

    while (!backendReady.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // Proxy
    EventLoop loop;
    UdpProxyServer proxy(&loop, proxyPort, "TestUdpProxy");
    proxy.AddBackend("127.0.0.1", backendPort, 1);
    proxy.SetIdleTimeout(1.0, 0.2);
    proxy.Start();

    // Client sender/receiver
    std::thread client([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        int cfd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        assert(cfd >= 0);

        sockaddr_in paddr;
        std::memset(&paddr, 0, sizeof(paddr));
        paddr.sin_family = AF_INET;
        paddr.sin_port = htons(proxyPort);
        assert(::inet_pton(AF_INET, "127.0.0.1", &paddr.sin_addr) == 1);

        std::string msg = "hello-udp";
        ssize_t s = ::sendto(cfd, msg.data(), msg.size(), 0, reinterpret_cast<sockaddr*>(&paddr), sizeof(paddr));
        assert(s == static_cast<ssize_t>(msg.size()));

        char buf[2048];
        sockaddr_in from;
        socklen_t len = sizeof(from);
        std::memset(&from, 0, sizeof(from));

        // Wait up to ~1s
        for (int i = 0; i < 200; ++i) {
            ssize_t n = ::recvfrom(cfd, buf, sizeof(buf), MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&from), &len);
            if (n > 0) {
                std::string got(buf, buf + n);
                assert(got == msg);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        ::close(cfd);
        backendStop.store(true);
        loop.Quit();
    });

    loop.Loop();
    client.join();
    backend.join();
    return 0;
}

