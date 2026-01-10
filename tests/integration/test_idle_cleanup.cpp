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

    constexpr uint16_t port = 9983;
    EventLoop loop;
    TcpServer server(&loop, InetAddress(port), "IdleCleanupServer");
    server.SetIdleTimeout(/*idle*/ 0.2, /*tick*/ 0.1);
    server.Start();

    std::thread client([&loop]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        int fd = connectTo(port);

        // No traffic; wait long enough to exceed idle timeout.
        std::this_thread::sleep_for(std::chrono::milliseconds(450));

        pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN | POLLHUP | POLLERR;

        bool gotEvent = false;
        for (int i = 0; i < 10; ++i) {
            int pret = ::poll(&pfd, 1, 200);
            assert(pret >= 0);
            if (pret == 1 && (pfd.revents & (POLLHUP | POLLERR | POLLIN)) != 0) {
                gotEvent = true;
                break;
            }
        }
        assert(gotEvent);

        char buf[8];
        ssize_t n = ::recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
        // Expect closed by server.
        assert(n == 0 || (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)));

        ::close(fd);
        loop.Quit();
    });

    loop.Loop();
    client.join();
    return 0;
}
