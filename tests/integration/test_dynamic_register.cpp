#include "proxy/ProxyServer.h"
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

using proxy::network::EventLoop;

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

static std::string httpRequest(uint16_t port, const std::string& raw) {
    int fd = connectTo(port);
    ssize_t n = ::send(fd, raw.data(), raw.size(), 0);
    assert(n == (ssize_t)raw.size());
    std::string resp = recvUntilClose(fd);
    ::close(fd);
    return resp;
}

int main() {
    proxy::common::Logger::Instance().SetLevel(proxy::common::LogLevel::ERROR);

    constexpr uint16_t proxyPort = 9986;
    EventLoop loop;
    proxy::ProxyServer server(&loop, proxyPort, "queue", "DynRegProxy");
    server.EnableAutoWeightAdjust(true);
    server.Start();

    std::thread client([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // Initially no backends: /stats should work.
        std::string stats0 = httpRequest(
            proxyPort,
            "GET /stats HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
        assert(stats0.find("200 OK") != std::string::npos);

        // Register two backends.
        {
            std::string body = "{\"ip\":\"127.0.0.1\",\"port\":9901,\"weight\":10}";
            std::string req = "POST /admin/backend_register HTTP/1.1\r\nHost: test\r\nContent-Type: application/json\r\nContent-Length: " +
                              std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
            std::string resp = httpRequest(proxyPort, req);
            assert(resp.find("200 OK") != std::string::npos);
        }
        {
            std::string body = "{\"ip\":\"127.0.0.1\",\"port\":9902,\"weight\":10}";
            std::string req = "POST /admin/backend_register HTTP/1.1\r\nHost: test\r\nContent-Type: application/json\r\nContent-Length: " +
                              std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
            std::string resp = httpRequest(proxyPort, req);
            assert(resp.find("200 OK") != std::string::npos);
        }

        // Provide metrics to adjust weights.
        {
            std::string body = "{\"backend\":\"127.0.0.1:9901\",\"queue_len\":50,\"gpu_util\":0.9}";
            std::string req = "POST /admin/backend_metrics HTTP/1.1\r\nHost: test\r\nContent-Type: application/json\r\nContent-Length: " +
                              std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
            std::string resp = httpRequest(proxyPort, req);
            assert(resp.find("200 OK") != std::string::npos);
        }
        {
            std::string body = "{\"backend\":\"127.0.0.1:9902\",\"queue_len\":0,\"gpu_util\":0.1}";
            std::string req = "POST /admin/backend_metrics HTTP/1.1\r\nHost: test\r\nContent-Type: application/json\r\nContent-Length: " +
                              std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
            std::string resp = httpRequest(proxyPort, req);
            assert(resp.find("200 OK") != std::string::npos);
        }

        // Verify /stats contains both backends and base_weight fields.
        std::string stats = httpRequest(
            proxyPort,
            "GET /stats HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
        assert(stats.find("127.0.0.1:9901") != std::string::npos);
        assert(stats.find("127.0.0.1:9902") != std::string::npos);
        assert(stats.find("\"base_weight\": 10") != std::string::npos);

        // Offline backend2.
        {
            std::string body = "{\"backend\":\"127.0.0.1:9902\",\"online\":0}";
            std::string req = "POST /admin/backend_online HTTP/1.1\r\nHost: test\r\nContent-Type: application/json\r\nContent-Length: " +
                              std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
            std::string resp = httpRequest(proxyPort, req);
            assert(resp.find("200 OK") != std::string::npos);
        }

        // Remove backend1 and verify /stats no longer lists it.
        {
            std::string body = "{\"backend\":\"127.0.0.1:9901\"}";
            std::string req = "POST /admin/backend_remove HTTP/1.1\r\nHost: test\r\nContent-Type: application/json\r\nContent-Length: " +
                              std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
            std::string resp = httpRequest(proxyPort, req);
            assert(resp.find("200 OK") != std::string::npos);
        }
        std::string stats1 = httpRequest(
            proxyPort,
            "GET /stats HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
        assert(stats1.find("127.0.0.1:9901") == std::string::npos);

        loop.QueueInLoop([&]() { loop.Quit(); });
    });

    loop.Loop();
    client.join();
    return 0;
}
