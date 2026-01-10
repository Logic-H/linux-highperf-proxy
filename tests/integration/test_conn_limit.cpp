#include "proxy/network/TcpServer.h"
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

using namespace proxy::common;
using namespace proxy::network;

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

int main() {
    Logger::Instance().SetLevel(LogLevel::INFO);

    constexpr uint16_t port = 9982;
    EventLoop loop;
    TcpServer server(&loop, InetAddress(port), "ConnLimitServer");
    server.SetMaxConnections(1);

    server.Start();

    std::thread client([&loop]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        int fd1 = connectTo(port);

        // Second connection should be rejected (accepted then immediately closed by server).
        int fd2 = connectTo(port);

        pollfd pfd;
        pfd.fd = fd2;
        pfd.events = POLLIN | POLLHUP | POLLERR;
        int pret = ::poll(&pfd, 1, 800);
        assert(pret == 1);
        assert((pfd.revents & (POLLHUP | POLLERR | POLLIN)) != 0);

        char buf[8];
        ssize_t n = ::recv(fd2, buf, sizeof(buf), MSG_DONTWAIT);
        // Most common case: server closed => recv returns 0.
        assert(n == 0 || (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)));

        ::close(fd2);
        ::close(fd1);

        loop.Quit();
    });

    loop.Loop();
    client.join();
    return 0;
}
