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

int main() {
    Logger::Instance().SetLevel(LogLevel::INFO);
    const uint16_t port = pickFreePort();
    EventLoop loop;
    HttpServer server(&loop, InetAddress(port), "GeminiHttpServer");
    server.setHttpCallback([&](const HttpRequest& req, HttpResponse* resp) {
        LOG_INFO << "HttpServer - Request: " << req.path();

        if (req.path() == "/") {
            resp->setStatusCode(HttpResponse::k200Ok);
            resp->setStatusMessage("OK");
            resp->setContentType("text/html");
            resp->addHeader("Server", "GeminiProxy");
            resp->setBody("<html><head><title>GeminiProxy</title></head>"
                          "<body><h1>Hello from GeminiProxy</h1></body></html>");
        } else if (req.path() == "/hello") {
            resp->setStatusCode(HttpResponse::k200Ok);
            resp->setStatusMessage("OK");
            resp->setContentType("text/plain");
            resp->setBody("Hello World!");
        } else if (req.path() == "/quit") {
            resp->setStatusCode(HttpResponse::k200Ok);
            resp->setStatusMessage("OK");
            resp->setContentType("text/plain");
            resp->setBody("Server Quitting...");
            loop.QueueInLoop([&]() { loop.Quit(); });
        } else {
            resp->setStatusCode(HttpResponse::k404NotFound);
            resp->setStatusMessage("Not Found");
        }
    });
    server.start();

    std::thread client([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        int fd = connectTo(port);
        const char* req =
            "GET / HTTP/1.1\r\n"
            "Host: test\r\n"
            "Connection: keep-alive\r\n"
            "\r\n"
            "GET /hello HTTP/1.1\r\n"
            "Host: test\r\n"
            "Connection: keep-alive\r\n"
            "\r\n"
            "GET /quit HTTP/1.1\r\n"
            "Host: test\r\n"
            "Connection: close\r\n"
            "\r\n";
        ssize_t n = ::send(fd, req, std::strlen(req), 0);
        assert(n == (ssize_t)std::strlen(req));

        std::string resp = recvUntilClose(fd);
        ::close(fd);

        assert(resp.find("HTTP/1.1 200 OK") != std::string::npos);
        assert(resp.find("Hello from GeminiProxy") != std::string::npos);
        assert(resp.find("Hello World!") != std::string::npos);
        assert(resp.find("Server Quitting...") != std::string::npos);
    });

    loop.Loop();
    client.join();
    return 0;
}
