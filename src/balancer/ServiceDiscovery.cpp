#include "proxy/balancer/ServiceDiscovery.h"
#include "proxy/common/Logger.h"
#include "proxy/network/InetAddress.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>
#include <optional>
#include <sstream>

namespace proxy {
namespace balancer {

namespace {

struct UrlParts {
    std::string scheme;
    std::string host;
    uint16_t port{0};
    std::string pathPrefix;
};

static bool startsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

static std::string trim(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    size_t j = s.size();
    while (j > i && std::isspace(static_cast<unsigned char>(s[j - 1]))) --j;
    return s.substr(i, j - i);
}

static std::optional<UrlParts> parseUrl(const std::string& url) {
    // Minimal: http://host:port[/prefix]
    UrlParts u;
    if (startsWith(url, "http://")) {
        u.scheme = "http";
        u.port = 80;
        u.pathPrefix = "/";
        std::string rest = url.substr(std::strlen("http://"));
        size_t slash = rest.find('/');
        std::string hostport = (slash == std::string::npos) ? rest : rest.substr(0, slash);
        u.pathPrefix = (slash == std::string::npos) ? "/" : rest.substr(slash);
        size_t colon = hostport.rfind(':');
        if (colon != std::string::npos) {
            u.host = hostport.substr(0, colon);
            try {
                int p = std::stoi(hostport.substr(colon + 1));
                if (p > 0 && p <= 65535) u.port = static_cast<uint16_t>(p);
            } catch (...) {
            }
        } else {
            u.host = hostport;
        }
        if (u.host.empty()) return std::nullopt;
        return u;
    }
    // https not supported in core (no TLS dependency).
    return std::nullopt;
}

static bool pollFd(int fd, short events, int timeoutMs) {
    pollfd pfd;
    pfd.fd = fd;
    pfd.events = events;
    pfd.revents = 0;
    const int r = ::poll(&pfd, 1, timeoutMs);
    return r == 1;
}

static bool connectWithTimeout(const std::string& host, uint16_t port, double timeoutSec, int* outFd) {
    *outFd = -1;
    addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const int gai = ::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res);
    if (gai != 0 || !res) return false;

    int fd = ::socket(res->ai_family, res->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, res->ai_protocol);
    if (fd < 0) {
        ::freeaddrinfo(res);
        return false;
    }
    const int rc = ::connect(fd, res->ai_addr, res->ai_addrlen);
    const int e = (rc == 0) ? 0 : errno;
    ::freeaddrinfo(res);

    if (rc == 0) {
        *outFd = fd;
        return true;
    }
    if (e != EINPROGRESS) {
        ::close(fd);
        return false;
    }
    const int timeoutMs = static_cast<int>(timeoutSec * 1000.0);
    if (!pollFd(fd, POLLOUT, timeoutMs)) {
        ::close(fd);
        return false;
    }
    int soerr = 0;
    socklen_t sl = sizeof(soerr);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) != 0 || soerr != 0) {
        ::close(fd);
        return false;
    }
    *outFd = fd;
    return true;
}

static bool sendAll(int fd, const std::string& s, double timeoutSec) {
    size_t off = 0;
    const int timeoutMs = static_cast<int>(timeoutSec * 1000.0);
    while (off < s.size()) {
        if (!pollFd(fd, POLLOUT, timeoutMs)) return false;
        const ssize_t n = ::send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
        if (n > 0) {
            off += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        return false;
    }
    return true;
}

static bool readAll(int fd, std::string* out, double timeoutSec) {
    const int timeoutMs = static_cast<int>(timeoutSec * 1000.0);
    out->clear();
    char buf[65536];
    while (true) {
        if (!pollFd(fd, POLLIN, timeoutMs)) break;
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n > 0) {
            out->append(buf, buf + n);
            continue;
        }
        break;
    }
    return !out->empty();
}

static bool httpRequest(const UrlParts& base,
                        const std::string& method,
                        const std::string& path,
                        const std::string& body,
                        double timeoutSec,
                        int* outStatus,
                        std::string* outBody) {
    int fd = -1;
    if (!connectWithTimeout(base.host, base.port, timeoutSec, &fd)) return false;

    std::ostringstream oss;
    oss << method << " " << path << " HTTP/1.1\r\n"
        << "Host: " << base.host << "\r\n"
        << "Connection: close\r\n";
    if (!body.empty()) {
        oss << "Content-Type: application/json\r\n"
            << "Content-Length: " << body.size() << "\r\n";
    }
    oss << "\r\n";
    if (!body.empty()) oss << body;
    const std::string req = oss.str();
    const bool okSend = sendAll(fd, req, timeoutSec);
    std::string resp;
    const bool okRead = okSend && readAll(fd, &resp, timeoutSec);
    ::close(fd);
    if (!okRead) return false;

    // Parse status and body (best-effort).
    size_t lineEnd = resp.find("\r\n");
    if (lineEnd == std::string::npos) return false;
    const std::string statusLine = resp.substr(0, lineEnd);
    int code = 0;
    {
        size_t sp1 = statusLine.find(' ');
        if (sp1 == std::string::npos) return false;
        size_t sp2 = statusLine.find(' ', sp1 + 1);
        std::string codeStr = (sp2 == std::string::npos) ? statusLine.substr(sp1 + 1) : statusLine.substr(sp1 + 1, sp2 - sp1 - 1);
        try {
            code = std::stoi(codeStr);
        } catch (...) {
            code = 0;
        }
    }
    size_t headerEnd = resp.find("\r\n\r\n");
    if (headerEnd == std::string::npos) headerEnd = resp.size();
    std::string bodyOut = (headerEnd + 4 <= resp.size()) ? resp.substr(headerEnd + 4) : std::string();
    if (outStatus) *outStatus = code;
    if (outBody) *outBody = std::move(bodyOut);
    return code > 0;
}

static std::string b64Alphabet() {
    return "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
}

static std::string b64encode(const std::string& in) {
    static const std::string abc = b64Alphabet();
    std::string out;
    int val = 0;
    int valb = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            out.push_back(abc[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back(abc[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

static std::string b64decode(const std::string& in) {
    static const std::string abc = b64Alphabet();
    std::vector<int> T(256, -1);
    for (int i = 0; i < 64; i++) T[static_cast<unsigned char>(abc[i])] = i;
    std::string out;
    int val = 0;
    int valb = -8;
    for (unsigned char c : in) {
        if (T[c] == -1) break;
        val = (val << 6) + T[c];
        valb += 6;
        if (valb >= 0) {
            out.push_back(char((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

static std::vector<DiscoveredBackend> parseIpPortList(const std::string& s, int defaultWeight) {
    std::vector<DiscoveredBackend> out;
    std::string cur;
    auto flushOne = [&](std::string item) {
        item = trim(item);
        if (item.empty()) return;
        size_t colon = item.rfind(':');
        if (colon == std::string::npos) return;
        std::string ip = trim(item.substr(0, colon));
        int port = 0;
        try {
            port = std::stoi(trim(item.substr(colon + 1)));
        } catch (...) {
            port = 0;
        }
        if (port <= 0 || port > 65535 || ip.empty()) return;
        DiscoveredBackend b;
        b.ip = std::move(ip);
        b.port = static_cast<uint16_t>(port);
        b.weight = std::max(1, defaultWeight);
        out.push_back(std::move(b));
    };

    for (char c : s) {
        if (c == ',' || c == '\n' || c == '\r') {
            flushOne(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    flushOne(cur);
    return out;
}

static std::vector<DiscoveredBackend> parseConsulHealthService(const std::string& json, int defaultWeight) {
    // Best-effort JSON scanning for "Address":"x" and "Port":y under Service.
    std::vector<DiscoveredBackend> out;
    size_t pos = 0;
    while (true) {
        size_t a = json.find("\"Address\"", pos);
        if (a == std::string::npos) break;
        size_t q1 = json.find('"', json.find(':', a) + 1);
        if (q1 == std::string::npos) break;
        size_t q2 = json.find('"', q1 + 1);
        if (q2 == std::string::npos) break;
        std::string ip = json.substr(q1 + 1, q2 - (q1 + 1));

        size_t p = json.find("\"Port\"", q2);
        if (p == std::string::npos) break;
        size_t c = json.find(':', p);
        if (c == std::string::npos) break;
        size_t end = c + 1;
        while (end < json.size() && (json[end] == ' ' || json[end] == '\t')) ++end;
        size_t end2 = end;
        while (end2 < json.size() && std::isdigit(static_cast<unsigned char>(json[end2]))) ++end2;
        int port = 0;
        try {
            port = std::stoi(json.substr(end, end2 - end));
        } catch (...) {
            port = 0;
        }
        pos = end2;
        if (ip.empty() || port <= 0 || port > 65535) continue;
        DiscoveredBackend b;
        b.ip = ip;
        b.port = static_cast<uint16_t>(port);
        b.weight = std::max(1, defaultWeight);
        out.push_back(std::move(b));
    }
    return out;
}

static std::vector<DiscoveredBackend> parseK8sEndpoints(const std::string& json, int defaultWeight) {
    // Best-effort scan for "ip":"x.x.x.x" and "port":y
    std::vector<std::string> ips;
    std::vector<int> ports;
    {
        size_t pos = 0;
        while (true) {
            size_t k = json.find("\"ip\"", pos);
            if (k == std::string::npos) break;
            size_t q1 = json.find('"', json.find(':', k) + 1);
            if (q1 == std::string::npos) break;
            size_t q2 = json.find('"', q1 + 1);
            if (q2 == std::string::npos) break;
            ips.push_back(json.substr(q1 + 1, q2 - (q1 + 1)));
            pos = q2 + 1;
        }
    }
    {
        size_t pos = 0;
        while (true) {
            size_t k = json.find("\"port\"", pos);
            if (k == std::string::npos) break;
            size_t c = json.find(':', k);
            if (c == std::string::npos) break;
            size_t i = c + 1;
            while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) ++i;
            size_t j = i;
            while (j < json.size() && std::isdigit(static_cast<unsigned char>(json[j]))) ++j;
            int p = 0;
            try {
                p = std::stoi(json.substr(i, j - i));
            } catch (...) {
                p = 0;
            }
            if (p > 0 && p <= 65535) ports.push_back(p);
            pos = j;
        }
    }
    std::vector<DiscoveredBackend> out;
    if (ports.empty()) return out;
    const int port = ports.front(); // choose first port
    for (const auto& ip : ips) {
        if (ip.empty()) continue;
        DiscoveredBackend b;
        b.ip = ip;
        b.port = static_cast<uint16_t>(port);
        b.weight = std::max(1, defaultWeight);
        out.push_back(std::move(b));
    }
    return out;
}

static std::vector<DiscoveredBackend> fetchConsul(const ServiceDiscoveryManager::Config& cfg) {
    auto baseOpt = parseUrl(cfg.consulUrl);
    if (!baseOpt) return {};
    const UrlParts base = *baseOpt;
    if (cfg.consulService.empty()) return {};
    std::string path = "/v1/health/service/" + cfg.consulService;
    if (cfg.consulPassingOnly) path += "?passing=true";
    int code = 0;
    std::string body;
    if (!httpRequest(base, "GET", path, "", cfg.timeoutSec, &code, &body)) return {};
    if (code != 200) return {};
    return parseConsulHealthService(body, cfg.defaultWeight);
}

static std::vector<DiscoveredBackend> fetchEtcd(const ServiceDiscoveryManager::Config& cfg) {
    auto baseOpt = parseUrl(cfg.etcdUrl);
    if (!baseOpt) return {};
    const UrlParts base = *baseOpt;
    if (cfg.etcdKey.empty()) return {};
    const std::string keyB64 = b64encode(cfg.etcdKey);
    const std::string req = std::string("{\"key\":\"") + keyB64 + "\"}";
    int code = 0;
    std::string body;
    if (!httpRequest(base, "POST", "/v3/kv/range", req, cfg.timeoutSec, &code, &body)) return {};
    if (code != 200) return {};
    // Find "value":"<b64>"
    size_t v = body.find("\"value\"");
    if (v == std::string::npos) return {};
    size_t q1 = body.find('"', body.find(':', v) + 1);
    if (q1 == std::string::npos) return {};
    size_t q2 = body.find('"', q1 + 1);
    if (q2 == std::string::npos) return {};
    const std::string valB64 = body.substr(q1 + 1, q2 - (q1 + 1));
    const std::string decoded = b64decode(valB64);
    return parseIpPortList(decoded, cfg.defaultWeight);
}

static std::vector<DiscoveredBackend> fetchK8s(const ServiceDiscoveryManager::Config& cfg) {
    // HTTPS not supported; allow http apiserver proxy or local test server.
    auto baseOpt = parseUrl(cfg.k8sUrl);
    if (!baseOpt) return {};
    const UrlParts base = *baseOpt;
    if (cfg.k8sEndpoints.empty()) return {};
    std::string path = "/api/v1/namespaces/" + cfg.k8sNamespace + "/endpoints/" + cfg.k8sEndpoints;
    int code = 0;
    std::string body;
    if (!httpRequest(base, "GET", path, "", cfg.timeoutSec, &code, &body)) return {};
    if (code != 200) return {};
    return parseK8sEndpoints(body, cfg.defaultWeight);
}

} // namespace

ServiceDiscoveryManager::ServiceDiscoveryManager(proxy::network::EventLoop* loop, BackendManager* backendManager, Config cfg)
    : loop_(loop), backendManager_(backendManager), cfg_(std::move(cfg)) {
}

ServiceDiscoveryManager::~ServiceDiscoveryManager() {
    Stop();
}

void ServiceDiscoveryManager::Start() {
    if (th_.joinable()) return;
    if (!backendManager_ || !loop_) return;
    stop_.store(false);
    th_ = std::thread([this]() { ThreadMain(); });
}

void ServiceDiscoveryManager::Stop() {
    stop_.store(true);
    if (th_.joinable()) th_.join();
}

void ServiceDiscoveryManager::ThreadMain() {
    while (!stop_.load()) {
        std::vector<DiscoveredBackend> items;
        (void)FetchOnce(&items);
        if (!items.empty()) {
            ApplyDiscovery(items);
        }
        const auto sleepMs = std::max(50, static_cast<int>(cfg_.intervalSec * 1000.0));
        for (int i = 0; i < sleepMs / 50 && !stop_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
}

bool ServiceDiscoveryManager::FetchOnce(std::vector<DiscoveredBackend>* out) {
    if (!out) return false;
    out->clear();
    if (cfg_.provider == "consul") {
        *out = fetchConsul(cfg_);
        return true;
    }
    if (cfg_.provider == "etcd") {
        *out = fetchEtcd(cfg_);
        return true;
    }
    if (cfg_.provider == "k8s") {
        *out = fetchK8s(cfg_);
        return true;
    }
    // off or unknown
    return true;
}

void ServiceDiscoveryManager::ApplyDiscovery(const std::vector<DiscoveredBackend>& items) {
    // Reconcile in loop thread.
    std::vector<DiscoveredBackend> copy = items;
    auto prev = managed_;
    loop_->QueueInLoop([this, copy = std::move(copy), prev = std::move(prev)]() mutable {
        std::unordered_set<std::string> now;
        now.reserve(copy.size());
        for (const auto& b : copy) {
            backendManager_->AddBackend(b.ip, b.port, std::max(1, b.weight));
            const std::string id = b.ip + ":" + std::to_string(b.port);
            now.insert(id);
            backendManager_->SetBackendOnline(id, true);
        }
        // Mark previously managed but absent nodes offline.
        for (const auto& id : prev) {
            if (now.find(id) == now.end()) {
                backendManager_->SetBackendOnline(id, false);
            }
        }
        managed_ = std::move(now);
    });
}

} // namespace balancer
} // namespace proxy
