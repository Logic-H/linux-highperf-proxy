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
#include <mutex>
#include <set>
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
    LOG_INFO << "Main thread ID: " << std::this_thread::get_id();
    
    constexpr uint16_t port = 8001;
    EventLoop loop;
    HttpServer server(&loop, InetAddress(port), "MT-HttpServer");

    std::mutex mu;
    std::set<std::thread::id> threads;
    std::atomic<int> handled{0};
    constexpr int kRequests = 6;

    server.setHttpCallback([&](const HttpRequest& req, HttpResponse* resp) {
        {
            std::lock_guard<std::mutex> lk(mu);
            threads.insert(std::this_thread::get_id());
        }
        LOG_INFO << "Request on thread " << std::this_thread::get_id() << ", path: " << req.path();
        resp->setStatusCode(HttpResponse::k200Ok);
        resp->setStatusMessage("OK");
        resp->setBody("Hello from multi-threaded server!");

        int n = ++handled;
        if (n >= kRequests) {
            loop.QueueInLoop([&]() { loop.Quit(); });
        }
    });
    
    server.setThreadNum(3); // 3 IO threads + 1 Main thread
    server.start();

    std::thread client([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        for (int i = 0; i < kRequests; ++i) {
            int fd = connectTo(port);
            const char* req =
                "GET /hello HTTP/1.1\r\n"
                "Host: test\r\n"
                "Connection: close\r\n"
                "\r\n";
            ssize_t n = ::send(fd, req, std::strlen(req), 0);
            assert(n == (ssize_t)std::strlen(req));
            std::string resp = recvUntilClose(fd);
            ::close(fd);
            assert(resp.find("HTTP/1.1 200 OK") != std::string::npos);
            assert(resp.find("Hello from multi-threaded server!") != std::string::npos);
        }
    });

    loop.Loop();
    client.join();

    {
        std::lock_guard<std::mutex> lk(mu);
        assert(!threads.empty());
        // Should observe requests handled by more than one IO thread.
        assert(threads.size() >= 2);
    }
    return 0;
}
