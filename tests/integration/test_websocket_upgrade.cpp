#include "proxy/ProxyServer.h"
#include "proxy/common/Logger.h"
#include "proxy/network/EventLoop.h"
 
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
 
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
 
using proxy::network::EventLoop;
 
namespace {
 
static uint32_t rol(uint32_t v, int bits) { return (v << bits) | (v >> (32 - bits)); }
 
static std::array<uint8_t, 20> sha1(const uint8_t* data, size_t len) {
    uint32_t h0 = 0x67452301u;
    uint32_t h1 = 0xEFCDAB89u;
    uint32_t h2 = 0x98BADCFEu;
    uint32_t h3 = 0x10325476u;
    uint32_t h4 = 0xC3D2E1F0u;
 
    std::vector<uint8_t> msg(data, data + len);
    msg.push_back(0x80);
    while ((msg.size() % 64) != 56) msg.push_back(0);
    uint64_t bitLen = static_cast<uint64_t>(len) * 8u;
    for (int i = 7; i >= 0; --i) msg.push_back(static_cast<uint8_t>((bitLen >> (i * 8)) & 0xff));
 
    for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            const size_t off = chunk + i * 4;
            w[i] = (static_cast<uint32_t>(msg[off]) << 24) |
                   (static_cast<uint32_t>(msg[off + 1]) << 16) |
                   (static_cast<uint32_t>(msg[off + 2]) << 8) |
                   (static_cast<uint32_t>(msg[off + 3]));
        }
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
 
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            uint32_t f = 0, k = 0;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            uint32_t temp = rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = temp;
        }
 
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }
 
    std::array<uint8_t, 20> out{};
    const uint32_t hs[5] = {h0, h1, h2, h3, h4};
    for (int i = 0; i < 5; ++i) {
        out[i * 4 + 0] = static_cast<uint8_t>((hs[i] >> 24) & 0xff);
        out[i * 4 + 1] = static_cast<uint8_t>((hs[i] >> 16) & 0xff);
        out[i * 4 + 2] = static_cast<uint8_t>((hs[i] >> 8) & 0xff);
        out[i * 4 + 3] = static_cast<uint8_t>((hs[i]) & 0xff);
    }
    return out;
}
 
static std::string base64Encode(const uint8_t* data, size_t len) {
    static const char* kTable = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = 0;
        int n = 0;
        for (int j = 0; j < 3; ++j) {
            v <<= 8;
            if (i + j < len) {
                v |= data[i + j];
                ++n;
            }
        }
        // v has n bytes in the lowest n*8 bits of the 24-bit block when built this way.
        int pad = 3 - n;
        for (int j = 0; j < 4 - pad; ++j) {
            int idx = (v >> (18 - j * 6)) & 0x3f;
            out.push_back(kTable[idx]);
        }
        for (int j = 0; j < pad; ++j) out.push_back('=');
    }
    return out;
}
 
static std::string wsAccept(const std::string& key) {
    static const char* kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string concat = key + kGuid;
    auto digest = sha1(reinterpret_cast<const uint8_t*>(concat.data()), concat.size());
    return base64Encode(digest.data(), digest.size());
}
 
static std::string extractHeader(const std::string& headers, const std::string& name) {
    const std::string needle = name + ":";
    size_t pos = headers.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    while (pos < headers.size() && (headers[pos] == ' ' || headers[pos] == '\t')) ++pos;
    size_t end = headers.find("\r\n", pos);
    if (end == std::string::npos) return {};
    return headers.substr(pos, end - pos);
}
 
static bool pollReadable(int fd, int timeoutMs) {
    pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN | POLLHUP | POLLERR;
    int ret = ::poll(&pfd, 1, timeoutMs);
    return ret == 1;
}
 
static std::string recvSome(int fd, int timeoutMs = 2000) {
    if (!pollReadable(fd, timeoutMs)) return {};
    char buf[4096];
    ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) return {};
    return std::string(buf, buf + n);
}
 
static void sendAll(int fd, const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
        ssize_t n = ::send(fd, s.data() + off, s.size() - off, 0);
        assert(n > 0);
        off += static_cast<size_t>(n);
    }
}
 
static std::string makeMaskedFrameText(const std::string& payload, std::array<uint8_t, 4> mask) {
    assert(payload.size() <= 125);
    std::string out;
    out.push_back(static_cast<char>(0x81)); // FIN + text
    out.push_back(static_cast<char>(0x80 | static_cast<uint8_t>(payload.size()))); // masked
    for (auto b : mask) out.push_back(static_cast<char>(b));
    for (size_t i = 0; i < payload.size(); ++i) {
        out.push_back(static_cast<char>(static_cast<uint8_t>(payload[i]) ^ mask[i % 4]));
    }
    return out;
}
 
static bool parseServerFrame(const std::string& data, std::string* payloadOut) {
    if (data.size() < 2) return false;
    uint8_t b0 = static_cast<uint8_t>(data[0]);
    uint8_t b1 = static_cast<uint8_t>(data[1]);
    const bool fin = (b0 & 0x80) != 0;
    const uint8_t opcode = b0 & 0x0f;
    const bool masked = (b1 & 0x80) != 0;
    uint64_t len = b1 & 0x7f;
    size_t off = 2;
    if (len == 126) {
        if (data.size() < off + 2) return false;
        len = (static_cast<uint8_t>(data[off]) << 8) | static_cast<uint8_t>(data[off + 1]);
        off += 2;
    } else if (len == 127) {
        return false; // not needed here
    }
    if (masked) return false; // server->client frames must be unmasked
    if (!fin) return false;
    if (opcode != 0x1 && opcode != 0x2) return false;
    if (data.size() < off + len) return false;
    payloadOut->assign(data.data() + off, data.data() + off + len);
    return true;
}
 
static void websocketEchoBackend(uint16_t port) {
    int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(lfd >= 0);
    int one = 1;
    ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
 
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    assert(::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1);
    assert(::bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    assert(::listen(lfd, 16) == 0);
 
    int cfd = ::accept(lfd, nullptr, nullptr);
    assert(cfd >= 0);
 
    std::string in;
    while (in.find("\r\n\r\n") == std::string::npos) {
        std::string chunk = recvSome(cfd, 2000);
        assert(!chunk.empty());
        in += chunk;
        if (in.size() > 64 * 1024) assert(false);
    }
    const size_t headerEnd = in.find("\r\n\r\n");
    const std::string headers = in.substr(0, headerEnd + 4);
    std::string key = extractHeader(headers, "Sec-WebSocket-Key");
    assert(!key.empty());
 
    std::string resp = "HTTP/1.1 101 Switching Protocols\r\n"
                       "Upgrade: websocket\r\n"
                       "Connection: Upgrade\r\n"
                       "Sec-WebSocket-Accept: " +
                       wsAccept(key) + "\r\n\r\n";
    sendAll(cfd, resp);
 
    std::string ws = in.substr(headerEnd + 4);
    while (ws.size() < 2) {
        std::string chunk = recvSome(cfd, 2000);
        assert(!chunk.empty());
        ws += chunk;
    }
    uint8_t b0 = static_cast<uint8_t>(ws[0]);
    uint8_t b1 = static_cast<uint8_t>(ws[1]);
    const bool fin = (b0 & 0x80) != 0;
    const uint8_t opcode = b0 & 0x0f;
    const bool masked = (b1 & 0x80) != 0;
    uint64_t len = b1 & 0x7f;
    size_t off = 2;
    assert(fin);
    assert(masked);
    assert(opcode == 0x1 || opcode == 0x2);
    assert(len <= 125);
    while (ws.size() < off + 4 + len) {
        std::string chunk = recvSome(cfd, 2000);
        assert(!chunk.empty());
        ws += chunk;
    }
    std::array<uint8_t, 4> mask{};
    for (int i = 0; i < 4; ++i) mask[i] = static_cast<uint8_t>(ws[off + i]);
    off += 4;
    std::string payload;
    payload.resize(static_cast<size_t>(len));
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<char>(static_cast<uint8_t>(ws[off + i]) ^ mask[i % 4]);
    }
 
    // Echo back as unmasked frame.
    std::string out;
    out.push_back(static_cast<char>(0x80 | opcode));
    out.push_back(static_cast<char>(static_cast<uint8_t>(payload.size())));
    out += payload;
    sendAll(cfd, out);
 
    ::shutdown(cfd, SHUT_RDWR);
    ::close(cfd);
    ::close(lfd);
}
 
static int connectTo(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    assert(::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1);
    assert(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    return fd;
}
 
static std::string recvUntil(int fd, const std::string& marker, int timeoutMs = 2000) {
    std::string out;
    auto start = std::chrono::steady_clock::now();
    while (out.find(marker) == std::string::npos) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        if (elapsed > timeoutMs) break;
        std::string chunk = recvSome(fd, 200);
        if (!chunk.empty()) out += chunk;
    }
    return out;
}
 
} // namespace
 
int main() {
    proxy::common::Logger::Instance().SetLevel(proxy::common::LogLevel::ERROR);
 
    constexpr uint16_t backendPort = 9913;
    constexpr uint16_t proxyPort = 9987;
 
    std::thread backend([&]() { websocketEchoBackend(backendPort); });
 
    EventLoop loop;
    proxy::ProxyServer server(&loop, proxyPort, "leastconn", "WsProxy");
    server.AddBackend("127.0.0.1", backendPort, 1);
    server.Start();
 
    std::thread client([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        int fd = connectTo(proxyPort);
 
        const std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
        std::string handshake =
            "GET /ws HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "Sec-WebSocket-Key: " +
            key + "\r\n\r\n";
 
        const std::string payload = "hello";
        std::array<uint8_t, 4> mask = {0x12, 0x34, 0x56, 0x78};
        std::string frame = makeMaskedFrameText(payload, mask);
 
        // Send handshake + first frame in one write to cover "tail bytes after headers" forwarding.
        sendAll(fd, handshake + frame);
 
        std::string resp = recvUntil(fd, "\r\n\r\n", 2000);
        assert(resp.find("101") != std::string::npos);
 
        // Receive server echo frame.
        std::string ws;
        {
            const size_t hdr = resp.find("\r\n\r\n");
            if (hdr != std::string::npos) {
                const size_t off = hdr + 4;
                if (off < resp.size()) ws = resp.substr(off);
            }
        }
        while (ws.size() < 2) ws += recvSome(fd, 2000);
        uint8_t b1 = static_cast<uint8_t>(ws[1]);
        size_t len = b1 & 0x7f;
        while (ws.size() < 2 + len) ws += recvSome(fd, 2000);
        std::string echoed;
        assert(parseServerFrame(ws, &echoed));
        assert(echoed == payload);
 
        ::close(fd);
        loop.QueueInLoop([&]() { loop.Quit(); });
    });
 
    loop.Loop();
    client.join();
    backend.join();
    return 0;
}
