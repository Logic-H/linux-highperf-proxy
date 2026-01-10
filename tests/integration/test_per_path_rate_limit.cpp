#include "proxy/ProxyServer.h"
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

static std::string recvAll(int fd, int timeoutMs = 1000) {
    std::string out;
    pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN | POLLHUP | POLLERR;
    while (true) {
        int pret = ::poll(&pfd, 1, timeoutMs);
        assert(pret == 1);
        char buf[4096];
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n > 0) {
            out.append(buf, buf + n);
            continue;
        }
        break;
    }
    return out;
}

static int parseStatus(const std::string& resp) {
    // "HTTP/1.1 200 OK"
    auto pos = resp.find("\r\n");
    if (pos == std::string::npos) return -1;
    std::string line = resp.substr(0, pos);
    auto sp = line.find(' ');
    if (sp == std::string::npos) return -1;
    auto sp2 = line.find(' ', sp + 1);
    std::string code = (sp2 == std::string::npos) ? line.substr(sp + 1) : line.substr(sp + 1, sp2 - sp - 1);
    if (code.size() != 3) return -1;
    return (code[0] - '0') * 100 + (code[1] - '0') * 10 + (code[2] - '0');
}

int main() {
    common::Logger::Instance().SetLevel(common::LogLevel::INFO);

    constexpr uint16_t port = 9980;
    network::EventLoop loop;
    ProxyServer server(&loop, port, "roundrobin", "PerPathRateLimitProxy");
    server.EnablePerPathRateLimit(1.0, 1.0, 60.0, 10000);
    server.Start();

    std::thread client([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        auto doReq = [&](int expectStatus) {
            int fd = connectTo(port);
            const char* req =
                "GET /stats HTTP/1.1\r\n"
                "Host: test\r\n"
                "Connection: close\r\n"
                "\r\n";
            ssize_t n = ::send(fd, req, std::strlen(req), 0);
            assert(n == (ssize_t)std::strlen(req));
            std::string resp = recvAll(fd);
            ::close(fd);
            int st = parseStatus(resp);
            assert(st == expectStatus);
        };

        doReq(200);
        doReq(429);

        loop.RunInLoop([&]() { loop.Quit(); });
    });

    loop.Loop();
    client.join();
    return 0;
}

