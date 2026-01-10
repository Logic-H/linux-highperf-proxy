#include "proxy/protocol/Cache.h"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <map>
#include <sstream>

namespace proxy {
namespace protocol {

namespace {

static uint64_t fnv1a64(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= static_cast<uint64_t>(c);
        h *= 1099511628211ull;
    }
    return h;
}

static std::string keyForBackend(const std::string& key) {
    // Memcached keys must not contain spaces/control characters; use a stable hash key for all backends.
    const uint64_t h = fnv1a64(key);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "p:%016llx", static_cast<unsigned long long>(h));
    return std::string(buf);
}

static bool pollFd(int fd, short events, int timeoutMs) {
    pollfd pfd;
    pfd.fd = fd;
    pfd.events = events;
    int ret = ::poll(&pfd, 1, timeoutMs);
    return ret == 1;
}

static int connectWithTimeout(const std::string& host, uint16_t port, int timeoutMs) {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
    if (fd < 0) return -1;

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        return -1;
    }

    int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc == 0) return fd;
    if (errno != EINPROGRESS) {
        ::close(fd);
        return -1;
    }
    if (!pollFd(fd, POLLOUT, timeoutMs)) {
        ::close(fd);
        return -1;
    }
    int err = 0;
    socklen_t len = sizeof(err);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

static bool sendAll(int fd, const std::string& data, int timeoutMs) {
    size_t off = 0;
    while (off < data.size()) {
        if (!pollFd(fd, POLLOUT, timeoutMs)) return false;
        ssize_t n = ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

static bool recvSome(int fd, std::string* out, size_t maxBytes, int timeoutMs) {
    if (!out) return false;
    if (!pollFd(fd, POLLIN, timeoutMs)) return false;
    char buf[4096];
    ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) return false;
    if (out->size() + static_cast<size_t>(n) > maxBytes) return false;
    out->append(buf, buf + n);
    return true;
}

static bool recvUntil(int fd, const std::string& delim, std::string* out, size_t maxBytes, int timeoutMs) {
    if (!out) return false;
    while (out->find(delim) == std::string::npos) {
        if (out->size() >= maxBytes) return false;
        if (!recvSome(fd, out, maxBytes, timeoutMs)) return false;
    }
    return true;
}

static bool readLineCrlf(int fd, std::string* lineOut, size_t maxBytes, int timeoutMs) {
    std::string buf;
    if (!recvUntil(fd, "\r\n", &buf, maxBytes, timeoutMs)) return false;
    const size_t pos = buf.find("\r\n");
    *lineOut = buf.substr(0, pos);
    // put back remaining? For simplicity, caller uses line-only reads on fresh connection.
    return true;
}

static bool redisGet(const Cache::Config& cfg, const std::string& key, std::string* valueOut) {
    if (!valueOut) return false;
    int fd = connectWithTimeout(cfg.host, cfg.port, cfg.timeoutMs);
    if (fd < 0) return false;

    const std::string k = keyForBackend(key);
    std::ostringstream oss;
    oss << "*2\r\n$3\r\nGET\r\n$" << k.size() << "\r\n" << k << "\r\n";
    const std::string cmd = oss.str();
    if (!sendAll(fd, cmd, cfg.timeoutMs)) {
        ::close(fd);
        return false;
    }

    std::string line;
    std::string buf;
    if (!recvUntil(fd, "\r\n", &buf, cfg.maxValueBytes, cfg.timeoutMs)) {
        ::close(fd);
        return false;
    }
    const size_t eol = buf.find("\r\n");
    line = buf.substr(0, eol);
    // Expect bulk string: $<len> or $-1
    if (line.size() < 2 || line[0] != '$') {
        ::close(fd);
        return false;
    }
    long long n = -1;
    try {
        n = std::stoll(line.substr(1));
    } catch (...) {
        ::close(fd);
        return false;
    }
    if (n < 0) {
        ::close(fd);
        return false; // miss
    }
    if (static_cast<size_t>(n) > cfg.maxValueBytes) {
        ::close(fd);
        return false;
    }

    // Consume header line + CRLF from buf.
    buf.erase(0, eol + 2);
    while (buf.size() < static_cast<size_t>(n) + 2) {
        if (!recvSome(fd, &buf, cfg.maxValueBytes, cfg.timeoutMs)) {
            ::close(fd);
            return false;
        }
    }
    *valueOut = buf.substr(0, static_cast<size_t>(n));
    ::close(fd);
    return true;
}

static bool redisSetex(const Cache::Config& cfg, const std::string& key, const std::string& value) {
    int fd = connectWithTimeout(cfg.host, cfg.port, cfg.timeoutMs);
    if (fd < 0) return false;
    const std::string ttl = std::to_string(cfg.ttlSec > 0 ? cfg.ttlSec : 60);
    const std::string k = keyForBackend(key);

    std::ostringstream oss;
    oss << "*4\r\n$5\r\nSETEX\r\n$" << k.size() << "\r\n" << k << "\r\n$" << ttl.size() << "\r\n" << ttl
        << "\r\n$" << value.size() << "\r\n";
    std::string cmd = oss.str();
    cmd += value;
    cmd += "\r\n";
    if (!sendAll(fd, cmd, cfg.timeoutMs)) {
        ::close(fd);
        return false;
    }
    std::string resp;
    if (!recvUntil(fd, "\r\n", &resp, 1024, cfg.timeoutMs)) {
        ::close(fd);
        return false;
    }
    ::close(fd);
    return resp.rfind("+OK", 0) == 0;
}

static bool memcachedGet(const Cache::Config& cfg, const std::string& key, std::string* valueOut) {
    if (!valueOut) return false;
    int fd = connectWithTimeout(cfg.host, cfg.port, cfg.timeoutMs);
    if (fd < 0) return false;
    const std::string k = keyForBackend(key);
    std::string cmd = "get " + k + "\r\n";
    if (!sendAll(fd, cmd, cfg.timeoutMs)) {
        ::close(fd);
        return false;
    }
    std::string resp;
    if (!recvUntil(fd, "END\r\n", &resp, cfg.maxValueBytes, cfg.timeoutMs)) {
        ::close(fd);
        return false;
    }
    // Miss: END\r\n
    if (resp == "END\r\n") {
        ::close(fd);
        return false;
    }
    // VALUE <key> <flags> <bytes>\r\n<data>\r\nEND\r\n
    const size_t lineEnd = resp.find("\r\n");
    if (lineEnd == std::string::npos) {
        ::close(fd);
        return false;
    }
    const std::string line = resp.substr(0, lineEnd);
    std::istringstream iss(line);
    std::string word;
    iss >> word;
    if (word != "VALUE") {
        ::close(fd);
        return false;
    }
    std::string gotKey;
    int flags = 0;
    size_t bytes = 0;
    iss >> gotKey >> flags >> bytes;
    (void)flags;
    if (bytes > cfg.maxValueBytes) {
        ::close(fd);
        return false;
    }
    const size_t dataStart = lineEnd + 2;
    if (resp.size() < dataStart + bytes + 2) {
        ::close(fd);
        return false;
    }
    *valueOut = resp.substr(dataStart, bytes);
    ::close(fd);
    return true;
}

static bool memcachedSet(const Cache::Config& cfg, const std::string& key, const std::string& value) {
    int fd = connectWithTimeout(cfg.host, cfg.port, cfg.timeoutMs);
    if (fd < 0) return false;
    const std::string k = keyForBackend(key);
    const int ttl = cfg.ttlSec > 0 ? cfg.ttlSec : 60;
    std::ostringstream oss;
    oss << "set " << k << " 0 " << ttl << " " << value.size() << "\r\n";
    std::string cmd = oss.str();
    cmd += value;
    cmd += "\r\n";
    if (!sendAll(fd, cmd, cfg.timeoutMs)) {
        ::close(fd);
        return false;
    }
    std::string resp;
    if (!recvUntil(fd, "\r\n", &resp, 1024, cfg.timeoutMs)) {
        ::close(fd);
        return false;
    }
    ::close(fd);
    return resp.rfind("STORED", 0) == 0;
}

} // namespace

void Cache::Configure(const Config& cfg) {
    cfg_ = cfg;
    if (cfg_.ttlSec <= 0) cfg_.ttlSec = 60;
    if (cfg_.timeoutMs <= 0) cfg_.timeoutMs = 5;
    if (cfg_.maxValueBytes < 1024) cfg_.maxValueBytes = 1024;
}

bool Cache::Get(const std::string& key, std::string* valueOut) const {
    if (!Enabled() || !valueOut) return false;
    if (key.empty()) return false;
    if (cfg_.backend == "redis") {
        return redisGet(cfg_, key, valueOut);
    }
    if (cfg_.backend == "memcached") {
        return memcachedGet(cfg_, key, valueOut);
    }
    return false;
}

void Cache::Set(const std::string& key, const std::string& value) const {
    if (!Enabled()) return;
    if (key.empty()) return;
    if (value.size() > cfg_.maxValueBytes) return;
    if (cfg_.backend == "redis") {
        (void)redisSetex(cfg_, key, value);
        return;
    }
    if (cfg_.backend == "memcached") {
        (void)memcachedSet(cfg_, key, value);
        return;
    }
}

} // namespace protocol
} // namespace proxy
