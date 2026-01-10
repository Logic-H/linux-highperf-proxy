#include "proxy/ProxyServer.h"
#include "proxy/common/Logger.h"
#include "proxy/network/EventLoop.h"

#include <arpa/inet.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

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

static int connectTo(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
        ::close(fd);
        return -1;
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

static std::optional<uint16_t> bindEphemeralPort(int* listenFdOut) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return std::nullopt;
    int opt = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return std::nullopt;
    }
    if (::listen(fd, 16) != 0) {
        ::close(fd);
        return std::nullopt;
    }
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        ::close(fd);
        return std::nullopt;
    }
    const uint16_t port = ntohs(addr.sin_port);
    if (port == 0) {
        ::close(fd);
        return std::nullopt;
    }
    *listenFdOut = fd;
    return port;
}

static std::optional<uint16_t> reserveFreePort() {
    int fd = -1;
    auto port = bindEphemeralPort(&fd);
    if (fd >= 0) ::close(fd);
    return port;
}

static bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return true;
}

static std::string headerValueCI(const std::string& head, const std::string& key) {
    size_t pos = 0;
    while (true) {
        size_t lineEnd = head.find("\r\n", pos);
        if (lineEnd == std::string::npos) break;
        const std::string line = head.substr(pos, lineEnd - pos);
        const size_t colon = line.find(':');
        if (colon != std::string::npos) {
            const std::string k = line.substr(0, colon);
            if (iequals(k, key)) {
                std::string v = line.substr(colon + 1);
                while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
                return v;
            }
        }
        pos = lineEnd + 2;
        if (pos >= head.size()) break;
    }
    return {};
}

static bool splitHttp(const std::string& raw, std::string* outHead, std::string* outBody) {
    const size_t pos = raw.find("\r\n\r\n");
    if (pos == std::string::npos) return false;
    *outHead = raw.substr(0, pos + 4);
    *outBody = raw.substr(pos + 4);
    return true;
}

struct BackendCaptured {
    std::string head;
    std::string body;
};

static void backendServer(int lfd, std::atomic<bool>* stop, BackendCaptured* cap, std::mutex* mu) {
    while (!stop->load()) {
        if (!pollReadable(lfd, 200)) continue;
        int cfd = ::accept(lfd, nullptr, nullptr);
        if (cfd < 0) continue;

        std::string in;
        while (!stop->load()) {
            if (!pollReadable(cfd, 200)) continue;
            char buf[4096];
            ssize_t n = ::recv(cfd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            in.append(buf, buf + n);
            size_t hdrEnd = in.find("\r\n\r\n");
            if (hdrEnd == std::string::npos) continue;
            std::string head = in.substr(0, hdrEnd + 4);
            size_t contentLen = 0;
            {
                size_t pos = head.find("\r\n");
                while (pos != std::string::npos) {
                    size_t next = head.find("\r\n", pos + 2);
                    std::string line = head.substr(pos + 2, (next == std::string::npos ? head.size() : next) - (pos + 2));
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (line.empty()) break;
                    const size_t colon = line.find(':');
                    if (colon != std::string::npos) {
                        std::string k = line.substr(0, colon);
                        std::string v = line.substr(colon + 1);
                        while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
                        if (iequals(k, "Content-Length")) {
                            contentLen = static_cast<size_t>(std::stoull(v));
                        }
                    }
                    pos = next;
                }
            }
            if (in.size() < hdrEnd + 4 + contentLen) continue;
            std::string body = in.substr(hdrEnd + 4, contentLen);
            in.erase(0, hdrEnd + 4 + contentLen);

            {
                std::lock_guard<std::mutex> lock(*mu);
                cap->head = head;
                cap->body = body;
            }

            const std::string respBody = "HELLO";
            const std::string resp = "HTTP/1.1 200 OK\r\n"
                                     "Content-Type: text/plain\r\n"
                                     "X-Backend: yes\r\n"
                                     "X-Remove-Me: 1\r\n"
                                     "Content-Length: " +
                                     std::to_string(respBody.size()) +
                                     "\r\n"
                                     "Connection: close\r\n"
                                     "\r\n" +
                                     respBody;
            sendAll(cfd, resp);
            break;
        }
        ::shutdown(cfd, SHUT_RDWR);
        ::close(cfd);
    }
    ::close(lfd);
}

static std::string recvAllWithTimeout(int fd, int timeoutMs) {
    auto start = std::chrono::steady_clock::now();
    std::string out;
    while (true) {
        if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() > timeoutMs) break;
        if (!pollReadable(fd, 200)) continue;
        char buf[8192];
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        out.append(buf, buf + n);
        if (out.find("\r\n\r\n") != std::string::npos) {
            std::string head, body;
            if (splitHttp(out, &head, &body)) {
                const std::string cl = headerValueCI(head, "Content-Length");
                if (!cl.empty()) {
                    const size_t need = static_cast<size_t>(std::stoull(cl));
                    if (body.size() >= need) break;
                }
            }
        }
    }
    return out;
}

} // namespace

int main() {
    ::signal(SIGPIPE, SIG_IGN);
    proxy::common::Logger::Instance().SetLevel(proxy::common::LogLevel::ERROR);

    int backendListenFd = -1;
    const auto backendPortOpt = bindEphemeralPort(&backendListenFd);
    const auto proxyPortOpt = reserveFreePort();
    assert(backendPortOpt.has_value());
    assert(proxyPortOpt.has_value());
    const uint16_t backendPort = *backendPortOpt;
    const uint16_t proxyPort = *proxyPortOpt;

    BackendCaptured captured;
    std::mutex capMu;
    std::atomic<bool> stopBackend{false};
    std::thread backend([&]() { backendServer(backendListenFd, &stopBackend, &captured, &capMu); });

    EventLoop loop;
    proxy::ProxyServer server(&loop, proxyPort, "roundrobin", "RewriteProxy");
    server.AddBackend("127.0.0.1", backendPort, 1);

    proxy::protocol::RewriteRule rule;
    rule.pathPrefix = "/rewrite";
    rule.method = proxy::protocol::HttpRequest::kPost;
    rule.reqSetHeaders["X-Req-Added"] = "yes";
    rule.reqDelHeaders.push_back("X-Req-Remove");
    rule.reqBodyReplaces.push_back({"PING", "PONG"});
    rule.respSetHeaders["X-From-Proxy"] = "1";
    rule.respDelHeaders.push_back("X-Remove-Me");
    rule.respBodyReplaces.push_back({"HELLO", "WORLD"});
    server.ConfigureRewriteRules({rule});

    server.Start();

    std::thread client([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        int fd = connectTo(proxyPort);
        assert(fd >= 0);
        const std::string body = "PING";
        std::string req = "POST /rewrite HTTP/1.1\r\n"
                          "Host: 127.0.0.1\r\n"
                          "X-Req-Remove: 1\r\n"
                          "Accept-Encoding: identity\r\n"
                          "Content-Length: " +
                          std::to_string(body.size()) +
                          "\r\n"
                          "Connection: close\r\n"
                          "\r\n";
        req += body;
        sendAll(fd, req);

        const std::string raw = recvAllWithTimeout(fd, 2000);
        std::string head, respBody;
        assert(splitHttp(raw, &head, &respBody));
        assert(head.find("200 OK") != std::string::npos);
        assert(respBody == "WORLD");
        assert(headerValueCI(head, "X-From-Proxy") == "1");
        assert(headerValueCI(head, "X-Remove-Me").empty());
        ::close(fd);

        loop.QueueInLoop([&]() { loop.Quit(); });
    });

    loop.Loop();
    client.join();

    stopBackend.store(true);
    backend.join();

    {
        std::lock_guard<std::mutex> lock(capMu);
        assert(captured.body == "PONG");
        assert(headerValueCI(captured.head, "X-Req-Added") == "yes");
        assert(headerValueCI(captured.head, "X-Req-Remove").empty());
    }

    return 0;
}

