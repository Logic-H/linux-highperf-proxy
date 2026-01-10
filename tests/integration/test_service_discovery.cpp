#include "proxy/balancer/ServiceDiscovery.h"
#include "proxy/balancer/BackendManager.h"
#include "proxy/common/Logger.h"
#include "proxy/network/EventLoop.h"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using proxy::balancer::DiscoveredBackend;
using proxy::balancer::ServiceDiscoveryManager;
using proxy::common::Logger;
using proxy::network::EventLoop;

namespace {

static bool pollReadable(int fd, int timeoutMs) {
    pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN | POLLHUP | POLLERR;
    int ret = ::poll(&pfd, 1, timeoutMs);
    return ret == 1;
}

static void sendAll(int fd, const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
        ssize_t n = ::send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
        if (n <= 0) return;
        off += static_cast<size_t>(n);
    }
}

static std::string recvUntilHeaderEnd(int fd, int timeoutMs) {
    std::string in;
    while (in.find("\r\n\r\n") == std::string::npos) {
        if (!pollReadable(fd, timeoutMs)) break;
        char buf[4096];
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        in.append(buf, buf + n);
        if (in.size() > 256 * 1024) break;
    }
    return in;
}

static uint16_t bindEphemeralPort(int* listenFdOut) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    int opt = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    assert(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    assert(::listen(fd, 16) == 0);
    socklen_t len = sizeof(addr);
    assert(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    *listenFdOut = fd;
    return ntohs(addr.sin_port);
}

static std::string httpResp(int code, const std::string& body) {
    std::string status = (code == 200) ? "OK" : "ERR";
    std::string h = "HTTP/1.1 " + std::to_string(code) + " " + status + "\r\n"
                    "Content-Type: application/json\r\n"
                    "Content-Length: " + std::to_string(body.size()) + "\r\n"
                    "Connection: close\r\n"
                    "\r\n";
    return h + body;
}

static void httpServer(int lfd, std::atomic<bool>* stop) {
    while (!stop->load()) {
        if (!pollReadable(lfd, 200)) continue;
        int cfd = ::accept(lfd, nullptr, nullptr);
        if (cfd < 0) continue;
        std::string req = recvUntilHeaderEnd(cfd, 200);
        std::string firstLine;
        size_t lineEnd = req.find("\r\n");
        if (lineEnd != std::string::npos) firstLine = req.substr(0, lineEnd);

        std::string body;
        if (firstLine.find("GET /v1/health/service/demo") == 0) {
            body = R"([{"Service":{"Address":"127.0.0.1","Port":9001}},{"Service":{"Address":"127.0.0.1","Port":9002}}])";
            sendAll(cfd, httpResp(200, body));
        } else if (firstLine.find("POST /v3/kv/range") == 0) {
            // base64("127.0.0.1:9003,127.0.0.1:9004") = MTI3LjAuMC4xOjkwMDMsMTI3LjAuMC4xOjkwMDQ=
            body = R"({"kvs":[{"value":"MTI3LjAuMC4xOjkwMDMsMTI3LjAuMC4xOjkwMDQ="}]})";
            sendAll(cfd, httpResp(200, body));
        } else if (firstLine.find("GET /api/v1/namespaces/default/endpoints/demo") == 0) {
            body = R"({"subsets":[{"addresses":[{"ip":"127.0.0.1"},{"ip":"127.0.0.2"}],"ports":[{"port":9005}]}]})";
            sendAll(cfd, httpResp(200, body));
        } else {
            sendAll(cfd, httpResp(404, "{}"));
        }
        ::shutdown(cfd, SHUT_RDWR);
        ::close(cfd);
    }
    ::close(lfd);
}

static void assertHas(const std::vector<DiscoveredBackend>& v, const std::string& ip, uint16_t port) {
    for (const auto& b : v) {
        if (b.ip == ip && b.port == port) return;
    }
    assert(false);
}

} // namespace

int main() {
    Logger::Instance().SetLevel(proxy::common::LogLevel::ERROR);

    int lfd = -1;
    const uint16_t port = bindEphemeralPort(&lfd);
    std::atomic<bool> stop{false};
    std::thread th([&]() { httpServer(lfd, &stop); });

    EventLoop loop;
    proxy::balancer::BackendManager bm(&loop, "roundrobin");

    // Consul
    {
        ServiceDiscoveryManager::Config cfg;
        cfg.provider = "consul";
        cfg.consulUrl = "http://127.0.0.1:" + std::to_string(port);
        cfg.consulService = "demo";
        cfg.timeoutSec = 1.0;
        ServiceDiscoveryManager mgr(&loop, &bm, cfg);
        std::vector<DiscoveredBackend> out;
        assert(mgr.FetchOnce(&out));
        assert(out.size() >= 2);
        assertHas(out, "127.0.0.1", 9001);
        assertHas(out, "127.0.0.1", 9002);
    }

    // Etcd
    {
        ServiceDiscoveryManager::Config cfg;
        cfg.provider = "etcd";
        cfg.etcdUrl = "http://127.0.0.1:" + std::to_string(port);
        cfg.etcdKey = "/proxy/backends";
        cfg.timeoutSec = 1.0;
        ServiceDiscoveryManager mgr(&loop, &bm, cfg);
        std::vector<DiscoveredBackend> out;
        assert(mgr.FetchOnce(&out));
        assert(out.size() >= 2);
        assertHas(out, "127.0.0.1", 9003);
        assertHas(out, "127.0.0.1", 9004);
    }

    // K8s
    {
        ServiceDiscoveryManager::Config cfg;
        cfg.provider = "k8s";
        cfg.k8sUrl = "http://127.0.0.1:" + std::to_string(port);
        cfg.k8sNamespace = "default";
        cfg.k8sEndpoints = "demo";
        cfg.timeoutSec = 1.0;
        ServiceDiscoveryManager mgr(&loop, &bm, cfg);
        std::vector<DiscoveredBackend> out;
        assert(mgr.FetchOnce(&out));
        assert(out.size() >= 2);
        assertHas(out, "127.0.0.1", 9005);
        assertHas(out, "127.0.0.2", 9005);
    }

    stop.store(true);
    th.join();
    return 0;
}

