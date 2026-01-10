#include "proxy/ProxyServer.h"
#include "proxy/common/Logger.h"
#include "proxy/network/EventLoop.h"
#include "proxy/protocol/GrpcFramer.h"
#include "proxy/protocol/Hpack.h"
#include "proxy/protocol/Http2Common.h"
#include "proxy/protocol/ProtobufLite.h"

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
#include <unordered_map>
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

static void sendAll(int fd, const void* data, size_t len) {
    const char* p = static_cast<const char*>(data);
    size_t off = 0;
    while (off < len) {
        ssize_t n = ::send(fd, p + off, len - off, MSG_NOSIGNAL);
        if (n <= 0) return;
        off += static_cast<size_t>(n);
    }
}

static void sendAll(int fd, const std::string& s) {
    sendAll(fd, s.data(), s.size());
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

static void httpBackend(int lfd, std::atomic<bool>* stop) {
    std::vector<std::thread> workers;
    auto handleConn = [stop](int cfd) {
        std::string in;
        while (!stop->load()) {
            if (!pollReadable(cfd, 200)) continue;
            char buf[4096];
            ssize_t n = ::recv(cfd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            in.append(buf, buf + n);
            if (in.size() > 256 * 1024) break;
            while (true) {
                size_t hdrEnd = in.find("\r\n\r\n");
                if (hdrEnd == std::string::npos) break;
                std::string reqLine = in.substr(0, in.find("\r\n"));
                std::string body = "OK";
                if (reqLine.find(" /a ") != std::string::npos) body = "A";
                std::string resp = "HTTP/1.1 200 OK\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Content-Length: " +
                                   std::to_string(body.size()) +
                                   "\r\n"
                                   "Connection: keep-alive\r\n"
                                   "\r\n" +
                                   body;
                sendAll(cfd, resp);
                in.erase(0, hdrEnd + 4);
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
        if (out.find("\r\n\r\n") != std::string::npos) break;
    }
    return out;
}

struct Frame {
    proxy::protocol::Http2FrameHeader h;
    std::vector<uint8_t> p;
};

static bool tryParseFrame(std::vector<uint8_t>* buf, Frame* out) {
    if (!buf || !out) return false;
    if (buf->size() < 9) return false;
    uint32_t len = (static_cast<uint32_t>((*buf)[0]) << 16) | (static_cast<uint32_t>((*buf)[1]) << 8) | static_cast<uint32_t>((*buf)[2]);
    if (buf->size() < 9 + len) return false;
    proxy::protocol::Http2FrameHeader h;
    h.length = len;
    h.type = (*buf)[3];
    h.flags = (*buf)[4];
    uint32_t sid = (static_cast<uint32_t>((*buf)[5]) << 24) | (static_cast<uint32_t>((*buf)[6]) << 16) | (static_cast<uint32_t>((*buf)[7]) << 8) |
                   static_cast<uint32_t>((*buf)[8]);
    h.streamId = sid & 0x7FFFFFFF;
    out->h = h;
    out->p.assign(buf->begin() + 9, buf->begin() + 9 + len);
    buf->erase(buf->begin(), buf->begin() + 9 + len);
    return true;
}

static bool recvFrame(int fd, std::vector<uint8_t>* buf, Frame* out, int timeoutMs = 2000) {
    auto start = std::chrono::steady_clock::now();
    while (true) {
        if (tryParseFrame(buf, out)) return true;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() > timeoutMs) return false;
        if (!pollReadable(fd, 200)) continue;
        char tmp[8192];
        ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) return false;
        buf->insert(buf->end(), tmp, tmp + n);
    }
}

static std::vector<uint8_t> makeSettings(bool ack) {
    std::vector<uint8_t> out;
    proxy::protocol::Http2FrameHeader h;
    h.length = 0;
    h.type = static_cast<uint8_t>(proxy::protocol::Http2FrameType::kSettings);
    h.flags = ack ? 0x1 : 0;
    h.streamId = 0;
    proxy::protocol::WriteFrame(&out, h, nullptr, 0);
    return out;
}

static std::vector<uint8_t> makeHeaders(uint32_t streamId, const std::vector<proxy::protocol::Hpack::Header>& headers, bool endStream) {
    proxy::protocol::Hpack enc;
    auto block = enc.EncodeNoIndex(headers);
    std::vector<uint8_t> out;
    proxy::protocol::Http2FrameHeader h;
    h.length = static_cast<uint32_t>(block.size());
    h.type = static_cast<uint8_t>(proxy::protocol::Http2FrameType::kHeaders);
    h.flags = 0x4; // END_HEADERS
    if (endStream) h.flags |= 0x1;
    h.streamId = streamId;
    proxy::protocol::WriteFrame(&out, h, block.data(), block.size());
    return out;
}

static std::vector<uint8_t> makeData(uint32_t streamId, const std::string& payload, bool endStream) {
    std::vector<uint8_t> out;
    proxy::protocol::Http2FrameHeader h;
    h.length = static_cast<uint32_t>(payload.size());
    h.type = static_cast<uint8_t>(proxy::protocol::Http2FrameType::kData);
    h.flags = endStream ? 0x1 : 0;
    h.streamId = streamId;
    proxy::protocol::WriteFrame(&out, h, reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
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

    std::atomic<bool> stopBackend{false};
    std::thread backend([&]() { httpBackend(backendListenFd, &stopBackend); });

    EventLoop loop;
    proxy::ProxyServer server(&loop, proxyPort, "roundrobin", "ProtoConvProxy");
    server.AddBackend("127.0.0.1", backendPort, 1);
    server.Start();

    std::thread client([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // HTTP -> gRPC conversion: REST /api/echo/unary
        {
            int fd = connectTo(proxyPort);
            assert(fd >= 0);
            const std::string body = "{\"msg\":\"hi\"}";
            std::string req = "POST /api/echo/unary HTTP/1.1\r\n"
                              "Host: 127.0.0.1\r\n"
                              "Content-Type: application/json\r\n"
                              "Content-Length: " +
                              std::to_string(body.size()) +
                              "\r\n"
                              "Connection: close\r\n"
                              "\r\n" +
                              body;
            sendAll(fd, req);
            const std::string resp = recvAllWithTimeout(fd, 2000);
            assert(resp.find("200 OK") != std::string::npos);
            assert(resp.find("\"msg\":\"hi\"") != std::string::npos);
            ::close(fd);
        }

        // gRPC -> HTTP conversion: /proxy.Gateway/HttpUnary with msg="/a" routed to backend.
        {
            int fd = connectTo(proxyPort);
            assert(fd >= 0);
            sendAll(fd, proxy::protocol::kHttp2ConnectionPreface, proxy::protocol::kHttp2ConnectionPrefaceLen);
            auto s = makeSettings(false);
            sendAll(fd, s.data(), s.size());

            std::vector<uint8_t> buf;
            bool sawServerSettings = false;
            bool sawServerAck = false;
            while (!(sawServerSettings && sawServerAck)) {
                Frame f;
                assert(recvFrame(fd, &buf, &f, 2000));
                if (f.h.type == static_cast<uint8_t>(proxy::protocol::Http2FrameType::kSettings) && f.h.streamId == 0) {
                    if (f.h.flags & 0x1) {
                        sawServerAck = true;
                    } else {
                        sawServerSettings = true;
                        auto ack = makeSettings(true);
                        sendAll(fd, ack.data(), ack.size());
                    }
                }
            }

            std::string pb;
            proxy::protocol::ProtobufLite::EncodeStringField1("/a", &pb);
            std::string frame;
            proxy::protocol::GrpcFramer::EncodeMessage(pb, &frame);

            std::vector<proxy::protocol::Hpack::Header> h = {
                {":method", "POST"},
                {":path", "/proxy.Gateway/HttpUnary"},
                {":scheme", "http"},
                {":authority", "127.0.0.1"},
                {"content-type", "application/grpc"},
                {"te", "trailers"},
            };
            auto hdrs = makeHeaders(1, h, false);
            sendAll(fd, hdrs.data(), hdrs.size());
            auto d = makeData(1, frame, true);
            sendAll(fd, d.data(), d.size());

            proxy::protocol::Hpack dec;
            std::string data;
            std::string grpcStatus;
            bool gotHeaders = false;
            bool gotTrailers = false;
            while (!gotTrailers) {
                Frame f;
                assert(recvFrame(fd, &buf, &f, 3000));
                if (f.h.streamId != 1) continue;
                if (f.h.type == static_cast<uint8_t>(proxy::protocol::Http2FrameType::kHeaders)) {
                    std::vector<proxy::protocol::Hpack::Header> hs;
                    assert(dec.Decode(f.p.data(), f.p.size(), &hs));
                    bool hasStatus = false;
                    for (const auto& hh : hs) {
                        if (hh.name == ":status") {
                            hasStatus = true;
                            assert(hh.value == "200");
                        } else if (hh.name == "grpc-status") {
                            grpcStatus = hh.value;
                        }
                    }
                    if (hasStatus) gotHeaders = true;
                    if (f.h.flags & 0x1) gotTrailers = true;
                    continue;
                }
                if (f.h.type == static_cast<uint8_t>(proxy::protocol::Http2FrameType::kData)) {
                    data.append(reinterpret_cast<const char*>(f.p.data()), f.p.size());
                }
            }
            assert(gotHeaders);
            assert(grpcStatus == "0");
            std::vector<std::string> msgs;
            assert(proxy::protocol::GrpcFramer::DecodeMessages(reinterpret_cast<const uint8_t*>(data.data()), data.size(), &msgs));
            assert(msgs.size() == 1);
            std::string outText;
            assert(proxy::protocol::ProtobufLite::DecodeStringField1(reinterpret_cast<const uint8_t*>(msgs[0].data()), msgs[0].size(), &outText));
            assert(outText == "A");

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

