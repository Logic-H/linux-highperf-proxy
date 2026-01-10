#include "proxy/protocol/HttpServer.h"
#include "proxy/protocol/HttpRequest.h"
#include "proxy/protocol/HttpResponse.h"
#include "proxy/network/EventLoop.h"
#include "proxy/network/InetAddress.h"
#include "proxy/common/Logger.h"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

using namespace proxy::protocol;
using namespace proxy::network;
using namespace proxy::common;

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

static std::string recvUntilClose(int fd, int timeoutMs = 2000) {
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

int main() {
    Logger::Instance().SetLevel(LogLevel::INFO);

    constexpr uint16_t port = 9979;
    EventLoop loop;
    HttpServer server(&loop, InetAddress(port), "HttpKeepAliveChunkedTest");

    std::atomic<int> seen{0};
    server.setHttpCallback([&](const HttpRequest& req, HttpResponse* resp) {
        if (req.path() == "/echo") {
            assert(req.getMethod() == HttpRequest::kPost);
            assert(req.body() == "hello");
            resp->setStatusCode(HttpResponse::k200Ok);
            resp->setStatusMessage("OK");
            resp->setContentType("text/plain");
            resp->setBody("echo:" + req.body());
        } else if (req.path() == "/ok") {
            resp->setStatusCode(HttpResponse::k200Ok);
            resp->setStatusMessage("OK");
            resp->setContentType("text/plain");
            resp->setBody("ok");
        } else {
            resp->setStatusCode(HttpResponse::k404NotFound);
            resp->setStatusMessage("Not Found");
        }
        int n = ++seen;
        if (n == 2) {
            loop.Quit();
        }
    });
    server.start();

    std::thread client([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        int fd = connectTo(port);

        const char* req1 =
            "POST /echo HTTP/1.1\r\n"
            "Host: test\r\n"
            "Transfer-Encoding: chunked\r\n"
            "Connection: keep-alive\r\n"
            "\r\n"
            "5\r\n"
            "hello\r\n"
            "0\r\n"
            "\r\n";
        ssize_t n1 = ::send(fd, req1, std::strlen(req1), 0);
        assert(n1 == (ssize_t)std::strlen(req1));

        const char* req2 =
            "GET /ok HTTP/1.1\r\n"
            "Host: test\r\n"
            "Connection: close\r\n"
            "\r\n";
        ssize_t n2 = ::send(fd, req2, std::strlen(req2), 0);
        assert(n2 == (ssize_t)std::strlen(req2));

        std::string resp = recvUntilClose(fd);
        ::close(fd);

        // Two responses should be present.
        assert(resp.find("HTTP/1.1 200 OK") != std::string::npos);
        assert(resp.find("echo:hello") != std::string::npos);
        assert(resp.find("ok") != std::string::npos);
    });

    loop.Loop();
    client.join();

    assert(seen.load() == 2);
    return 0;
}

