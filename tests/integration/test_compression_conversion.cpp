#include "proxy/ProxyServer.h"
#include "proxy/common/Logger.h"
#include "proxy/network/EventLoop.h"
#include "proxy/protocol/Compression.h"

#include <arpa/inet.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <vector>

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

static void backendServer(int lfd, std::atomic<bool>* stop) {
    std::vector<std::thread> workers;
    auto handleConn = [stop](int cfd) {
        std::string in;
        while (!stop->load()) {
            if (!pollReadable(cfd, 200)) continue;
            char buf[4096];
            ssize_t n = ::recv(cfd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            in.append(buf, buf + n);
            if (in.size() > 512 * 1024) break;
            while (true) {
                size_t hdrEnd = in.find("\r\n\r\n");
                if (hdrEnd == std::string::npos) break;
                std::string head = in.substr(0, hdrEnd + 4);
                std::string reqLine = head.substr(0, head.find("\r\n"));

                // Parse Content-Length + Content-Encoding, then read body if present.
                size_t contentLen = 0;
                std::string contentEnc;
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
                            if (k == "Content-Length") {
                                contentLen = static_cast<size_t>(std::stoull(v));
                            } else if (k == "Content-Encoding") {
                                contentEnc = v;
                            }
                        }
                        pos = next;
                    }
                }

                if (in.size() < hdrEnd + 4 + contentLen) break;
                std::string body = in.substr(hdrEnd + 4, contentLen);
                in.erase(0, hdrEnd + 4 + contentLen);

                if (reqLine.find(" /plain ") != std::string::npos) {
                    const std::string bodyOut = "HELLO";
                    const std::string resp = "HTTP/1.1 200 OK\r\n"
                                             "Content-Type: text/plain\r\n"
                                             "Content-Length: " +
                                             std::to_string(bodyOut.size()) +
                                             "\r\n"
                                             "Connection: keep-alive\r\n"
                                             "\r\n" +
                                             bodyOut;
                    sendAll(cfd, resp);
                    continue;
                }
                if (reqLine.find(" /gzip ") != std::string::npos) {
                    std::string gz;
                    proxy::protocol::Compression::Compress(proxy::protocol::Compression::Encoding::kGzip, "HELLO", &gz);
                    std::string resp = "HTTP/1.1 200 OK\r\n"
                                       "Content-Type: text/plain\r\n"
                                       "Content-Encoding: gzip\r\n"
                                       "Content-Length: " +
                                       std::to_string(gz.size()) +
                                       "\r\n"
                                       "Connection: keep-alive\r\n"
                                       "\r\n";
                    resp += gz;
                    sendAll(cfd, resp);
                    continue;
                }
                if (reqLine.find(" /echo ") != std::string::npos) {
                    // If client sent gzipped body to backend, just echo as-is (proxy should have normalized to identity).
                    std::string resp = "HTTP/1.1 200 OK\r\n"
                                       "Content-Type: text/plain\r\n"
                                       "Content-Length: " +
                                       std::to_string(body.size()) +
                                       "\r\n"
                                       "Connection: keep-alive\r\n"
                                       "\r\n";
                    resp += body;
                    sendAll(cfd, resp);
                    continue;
                }
            }
        }
        ::shutdown(cfd, SHUT_RDWR);
        ::close(cfd);
    };

    while (!stop->load()) {
        if (!pollReadable(lfd, 200)) continue;
        int cfd = ::accept(lfd, nullptr, nullptr);
        if (cfd < 0) continue;
        workers.emplace_back([handleConn, cfd]() { handleConn(cfd); });
    }
    for (auto& t : workers) {
        if (t.joinable()) t.join();
    }
    ::close(lfd);
}

static std::string recvSome(int fd, int timeoutMs) {
    auto start = std::chrono::steady_clock::now();
    std::string out;
    while (true) {
        if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() > timeoutMs) break;
        if (!pollReadable(fd, 200)) continue;
        char buf[8192];
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        out.append(buf, buf + n);
        if (out.find("\r\n\r\n") != std::string::npos) break;
    }
    return out;
}

static bool splitHttp(const std::string& raw, std::string* outHead, std::string* outBody) {
    const size_t pos = raw.find("\r\n\r\n");
    if (pos == std::string::npos) return false;
    *outHead = raw.substr(0, pos + 4);
    *outBody = raw.substr(pos + 4);
    return true;
}

static std::string headerValue(const std::string& head, const std::string& key) {
    const std::string needle = key + ":";
    size_t pos = 0;
    while (true) {
        size_t lineEnd = head.find("\r\n", pos);
        if (lineEnd == std::string::npos) break;
        const std::string line = head.substr(pos, lineEnd - pos);
        if (line.rfind(needle, 0) == 0) {
            std::string v = line.substr(needle.size());
            while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
            return v;
        }
        pos = lineEnd + 2;
        if (pos >= head.size()) break;
    }
    return {};
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

    std::atomic<bool> stopBackend{false};
    std::thread backend([&]() { backendServer(backendListenFd, &stopBackend); });

    EventLoop loop;
    proxy::ProxyServer server(&loop, proxyPort, "roundrobin", "CompressConvProxy");
    server.AddBackend("127.0.0.1", backendPort, 1);
    server.Start();

    std::thread client([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // 1) Backend identity -> client gzip (proxy compresses).
        {
            int fd = connectTo(proxyPort);
            assert(fd >= 0);
            const std::string req = "GET /plain HTTP/1.1\r\n"
                                    "Host: 127.0.0.1\r\n"
                                    "Accept-Encoding: gzip\r\n"
                                    "Connection: close\r\n"
                                    "\r\n";
            sendAll(fd, req);
            std::string raw = recvSome(fd, 2000);
            char buf[8192];
            ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n > 0) raw.append(buf, buf + n);
            std::string head, body;
            assert(splitHttp(raw, &head, &body));
            assert(head.find("200 OK") != std::string::npos);
            assert(headerValue(head, "Content-Encoding") == "gzip");
            std::string dec;
            assert(proxy::protocol::Compression::Decompress(proxy::protocol::Compression::Encoding::kGzip, body, &dec));
            assert(dec == "HELLO");
            ::close(fd);
        }

        // 2) Backend gzip -> client identity (proxy decompresses).
        {
            int fd = connectTo(proxyPort);
            assert(fd >= 0);
            const std::string req = "GET /gzip HTTP/1.1\r\n"
                                    "Host: 127.0.0.1\r\n"
                                    "Accept-Encoding: identity\r\n"
                                    "Connection: close\r\n"
                                    "\r\n";
            sendAll(fd, req);
            std::string raw = recvSome(fd, 2000);
            char buf[8192];
            ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n > 0) raw.append(buf, buf + n);
            std::string head, body;
            assert(splitHttp(raw, &head, &body));
            assert(head.find("200 OK") != std::string::npos);
            assert(headerValue(head, "Content-Encoding").empty());
            assert(body == "HELLO");
            ::close(fd);
        }

        // 3) Client gzip request body -> backend identity (proxy decompresses request before forwarding).
        {
            int fd = connectTo(proxyPort);
            assert(fd >= 0);
            std::string gz;
            assert(proxy::protocol::Compression::Compress(proxy::protocol::Compression::Encoding::kGzip, "PING", &gz));
            std::string req = "POST /echo HTTP/1.1\r\n"
                              "Host: 127.0.0.1\r\n"
                              "Content-Encoding: gzip\r\n"
                              "Content-Length: " +
                              std::to_string(gz.size()) +
                              "\r\n"
                              "Connection: close\r\n"
                              "\r\n";
            req += gz;
            sendAll(fd, req);
            std::string raw = recvSome(fd, 2000);
            char buf[8192];
            ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n > 0) raw.append(buf, buf + n);
            std::string head, body;
            assert(splitHttp(raw, &head, &body));
            assert(head.find("200 OK") != std::string::npos);
            assert(body == "PING");
            ::close(fd);
        }

        loop.QueueInLoop([&]() { loop.Quit(); });
    });

    loop.Loop();
    client.join();

    stopBackend.store(true);
    backend.join();
    return 0;
}

