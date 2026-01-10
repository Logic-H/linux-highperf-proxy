#include "proxy/balancer/HttpHealthChecker.h"
#include "proxy/network/Acceptor.h"
#include "proxy/network/EventLoop.h"
#include "proxy/network/InetAddress.h"
#include "proxy/common/Logger.h"

#include <atomic>
#include <cstring>
#include <unistd.h>

using namespace proxy;

static void WriteAllAndClose(int fd, const char* data) {
    const size_t len = std::strlen(data);
    size_t off = 0;
    while (off < len) {
        const ssize_t n = ::write(fd, data + off, len - off);
        if (n > 0) {
            off += static_cast<size_t>(n);
            continue;
        }
        break;
    }
    ::close(fd);
}

int main() {
    common::Logger::Instance().SetLevel(common::LogLevel::INFO);
    network::EventLoop loop;

    const int okPort = 9997;
    const int badPort = 9996;

    network::Acceptor okAcceptor(&loop, network::InetAddress(okPort), true);
    okAcceptor.SetNewConnectionCallback([](int sockfd, const network::InetAddress&) {
        const char* resp =
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n"
            "\r\n";
        WriteAllAndClose(sockfd, resp);
    });
    okAcceptor.Listen();

    network::Acceptor badAcceptor(&loop, network::InetAddress(badPort), true);
    badAcceptor.SetNewConnectionCallback([](int sockfd, const network::InetAddress&) {
        const char* resp =
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n"
            "\r\n";
        WriteAllAndClose(sockfd, resp);
    });
    badAcceptor.Listen();

    auto checker = std::make_shared<balancer::HttpHealthChecker>(&loop, 1.0, "127.0.0.1", "/health");

    std::atomic<int> callbacks{0};
    std::atomic<int> okCount{0};

    checker->Check(network::InetAddress("127.0.0.1", okPort),
                  [&](bool healthy, const network::InetAddress& addr) {
                      LOG_INFO << "Check " << addr.toIpPort() << " healthy=" << healthy;
                      if (healthy) okCount++;
                      callbacks++;
                      if (callbacks == 2) loop.Quit();
                  });

    checker->Check(network::InetAddress("127.0.0.1", badPort),
                  [&](bool healthy, const network::InetAddress& addr) {
                      LOG_INFO << "Check " << addr.toIpPort() << " healthy=" << healthy;
                      if (!healthy) okCount++;
                      callbacks++;
                      if (callbacks == 2) loop.Quit();
                  });

    loop.Loop();

    if (okCount != 2) {
        LOG_ERROR << "HttpHealthChecker: FAIL";
        return 1;
    }
    LOG_INFO << "HttpHealthChecker: PASS";
    return 0;
}

