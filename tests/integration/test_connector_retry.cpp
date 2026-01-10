#include "proxy/network/Connector.h"
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
#include <thread>

using namespace proxy::common;
using namespace proxy::network;

static int startOneShotServer(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);

    int opt = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    assert(::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1);

    int ret = ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    assert(ret == 0);
    ret = ::listen(fd, 16);
    assert(ret == 0);
    return fd;
}

int main() {
    Logger::Instance().SetLevel(LogLevel::INFO);

    constexpr uint16_t port = 9984;
    std::atomic<bool> connected{false};

    EventLoop loop;
    Connector connector(&loop, InetAddress(std::string("127.0.0.1"), port));
    connector.SetNewConnectionCallback([&](int sockfd) {
        connected.store(true);
        ::close(sockfd);
        loop.Quit();
    });

    // 首次连接会失败(没有服务端)，随后应通过 timerfd 定时重试。
    connector.Start();

    std::thread serverThread([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(650));
        int listenFd = startOneShotServer(port);
        int connFd = ::accept(listenFd, nullptr, nullptr);
        if (connFd >= 0) {
            ::close(connFd);
        }
        ::close(listenFd);
    });

    loop.Loop();
    serverThread.join();

    assert(connected.load());
    return 0;
}

