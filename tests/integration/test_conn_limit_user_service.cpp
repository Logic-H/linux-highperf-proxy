#include "proxy/ProxyServer.h"
#include "proxy/network/Acceptor.h"
#include "proxy/network/EventLoop.h"
#include "proxy/network/InetAddress.h"
#include "proxy/common/Logger.h"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

using namespace proxy;

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

static void sendHttp(int fd, const std::string& path, const std::string& token) {
    std::string req = "GET " + path + " HTTP/1.1\r\n"
                      "Host: test\r\n"
                      "Connection: keep-alive\r\n"
                      "X-Api-Token: " +
                      token +
                      "\r\n"
                      "\r\n";
    ssize_t n = ::send(fd, req.data(), req.size(), 0);
    assert(n == (ssize_t)req.size());
}

static std::string recvSome(int fd, int timeoutMs = 1000) {
    pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN | POLLHUP | POLLERR;
    int pret = ::poll(&pfd, 1, timeoutMs);
    assert(pret == 1);
    char buf[4096];
    ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) return "";
    return std::string(buf, buf + n);
}

int main() {
    common::Logger::Instance().SetLevel(common::LogLevel::INFO);

    constexpr uint16_t backendPort = 9100;
    constexpr uint16_t proxyPort = 9981;

    network::EventLoop loop;

    // Dummy backend: accept and hold connections open.
    std::vector<int> backendFds;
    network::Acceptor backend(&loop, network::InetAddress(backendPort), true);
    backend.SetNewConnectionCallback([&](int fd, const network::InetAddress&) {
        backendFds.push_back(fd);
    });
    backend.Listen();

    ProxyServer proxy(&loop, proxyPort, "roundrobin", "ProxyConnLimitUserService");
    proxy.AddBackend("127.0.0.1", backendPort, 1);
    proxy.StartHealthCheck(5.0);

    proxy.SetMaxConnectionsPerUser(1, "X-Api-Token");
    proxy.SetMaxConnectionsPerService(1);
    proxy.Start();

    std::thread client([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // First connection (token A, service "svc") should be accepted and proxied (backend holds open).
        int fd1 = connectTo(proxyPort);
        sendHttp(fd1, "/svc/one", "A");

        // Second connection with same token A should be rejected by per-user limit.
        int fd2 = connectTo(proxyPort);
        sendHttp(fd2, "/svc/two", "A");
        std::string r2 = recvSome(fd2);
        assert(r2.find("429") != std::string::npos);
        ::close(fd2);

        // Third connection with different token B but same service "svc" should be rejected by per-service limit.
        int fd3 = connectTo(proxyPort);
        sendHttp(fd3, "/svc/three", "B");
        std::string r3 = recvSome(fd3);
        assert(r3.find("429") != std::string::npos);
        ::close(fd3);

        ::close(fd1);

        // Close backend fds and stop.
        loop.RunInLoop([&]() {
            for (int fd : backendFds) ::close(fd);
            backendFds.clear();
            loop.Quit();
        });
    });

    loop.Loop();
    client.join();
    return 0;
}
