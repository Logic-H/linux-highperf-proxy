#include "proxy/balancer/AiServiceChecker.h"
#include "proxy/common/Logger.h"
#include "proxy/network/EventLoopThread.h"
#include "proxy/network/InetAddress.h"

#include <arpa/inet.h>
#include <cassert>
#include <condition_variable>
#include <cstring>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

static int MakeListenSocket(uint16_t* outPort) {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    if (fd < 0) return -1;

    int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    if (::listen(fd, 16) != 0) {
        ::close(fd);
        return -1;
    }

    sockaddr_in bound;
    socklen_t len = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len) != 0) {
        ::close(fd);
        return -1;
    }
    *outPort = ntohs(bound.sin_port);
    return fd;
}

int main() {
    proxy::common::Logger::Instance().SetLevel(proxy::common::LogLevel::ERROR);

    uint16_t port = 0;
    int listenFd = MakeListenSocket(&port);
    assert(listenFd >= 0);

    std::atomic<bool> stop{false};
    std::thread serverThr([&]() {
        const std::string body = "{\"gpu_util\":0.5,\"vram_used_mb\":1024,\"vram_total_mb\":8192,"
                                 "\"model_loaded\":true,\"queue_len\":7}";
        while (!stop.load()) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(listenFd, &rfds);
            timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 100 * 1000;
            int rc = ::select(listenFd + 1, &rfds, nullptr, nullptr, &tv);
            if (rc <= 0) continue;

            sockaddr_in cli;
            socklen_t len = sizeof(cli);
            int cfd = ::accept4(listenFd, reinterpret_cast<sockaddr*>(&cli), &len, SOCK_CLOEXEC);
            if (cfd < 0) continue;

            char buf[2048];
            (void)::recv(cfd, buf, sizeof(buf), 0);

            std::string resp = "HTTP/1.1 200 OK\r\n"
                               "Content-Type: application/json\r\n"
                               "Content-Length: " +
                               std::to_string(body.size()) +
                               "\r\n"
                               "Connection: close\r\n"
                               "\r\n" +
                               body;
            (void)::send(cfd, resp.data(), resp.size(), MSG_NOSIGNAL);
            ::close(cfd);
        }
    });

    proxy::network::EventLoopThread loopThread("ai_service_check");
    proxy::network::EventLoop* loop = loopThread.StartLoop();

    auto checker = std::make_shared<proxy::balancer::AiServiceChecker>(loop, 1.0, "127.0.0.1", "/ai/status");
    proxy::balancer::AiServiceChecker::Result result;
    bool ok = false;
    bool done = false;
    std::mutex mu;
    std::condition_variable cv;

    checker->Check(proxy::network::InetAddress("127.0.0.1", port),
                   [&](bool o, const proxy::network::InetAddress&, const proxy::balancer::AiServiceChecker::Result& r) {
                       std::lock_guard<std::mutex> lock(mu);
                       ok = o;
                       result = r;
                       done = true;
                       cv.notify_one();
                   });

    {
        std::unique_lock<std::mutex> lock(mu);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!done) {
            if (cv.wait_until(lock, deadline) == std::cv_status::timeout) break;
        }
    }

    assert(done);
    assert(ok);
    assert(result.hasGpu);
    assert(result.vramTotalMb == 8192);
    assert(result.vramUsedMb == 1024);
    assert(result.gpuUtil01 > 0.0);
    assert(result.hasQueueLen);
    assert(result.queueLen == 7);
    assert(result.hasModelLoaded);
    assert(result.modelLoaded);

    stop.store(true);
    ::close(listenFd);
    serverThr.join();
    return 0;
}
