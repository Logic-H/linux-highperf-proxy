#include "proxy/protocol/TrafficMirror.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>

namespace proxy {
namespace protocol {

namespace {

static thread_local int tl_fd = -1;
static thread_local uint32_t tl_cfg_ver = 0;
static thread_local sockaddr_in tl_dst{};

static void closeThreadSocket() {
    if (tl_fd >= 0) {
        ::close(tl_fd);
        tl_fd = -1;
    }
}

} // namespace

void TrafficMirror::Configure(const Config& cfg) {
    cfg_ = cfg;
    // Clamp.
    if (cfg_.sampleRate < 0.0) cfg_.sampleRate = 0.0;
    if (cfg_.sampleRate > 1.0) cfg_.sampleRate = 1.0;
    if (cfg_.maxBytes < 256) cfg_.maxBytes = 256;
    if (cfg_.maxBodyBytes > cfg_.maxBytes) cfg_.maxBodyBytes = cfg_.maxBytes;
    cfgVersion_.fetch_add(1, std::memory_order_relaxed);
}

uint64_t TrafficMirror::NowUnixMs() {
    const auto now = std::chrono::system_clock::now();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
}

bool TrafficMirror::ShouldSample() {
    if (!cfg_.enabled || cfg_.udpPort == 0) return false;
    if (cfg_.sampleRate >= 1.0) return true;
    if (cfg_.sampleRate <= 0.0) return false;
    // xorshift32
    uint32_t x = rng_.load(std::memory_order_relaxed);
    x ^= (x << 13);
    x ^= (x >> 17);
    x ^= (x << 5);
    rng_.store(x, std::memory_order_relaxed);
    const double u01 = (static_cast<double>(x) / static_cast<double>(0xFFFFFFFFu));
    return u01 < cfg_.sampleRate;
}

void TrafficMirror::EnsureSocketForThread() {
    const uint32_t ver = cfgVersion_.load(std::memory_order_relaxed);
    if (tl_fd >= 0 && tl_cfg_ver == ver) return;

    closeThreadSocket();
    tl_cfg_ver = ver;
    std::memset(&tl_dst, 0, sizeof(tl_dst));

    if (!cfg_.enabled || cfg_.udpPort == 0) return;

    tl_fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_UDP);
    if (tl_fd < 0) {
        tl_fd = -1;
        return;
    }

    tl_dst.sin_family = AF_INET;
    tl_dst.sin_port = htons(cfg_.udpPort);
    if (::inet_pton(AF_INET, cfg_.udpHost.c_str(), &tl_dst.sin_addr) != 1) {
        closeThreadSocket();
        std::memset(&tl_dst, 0, sizeof(tl_dst));
        return;
    }
}

std::string TrafficMirror::JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    // Control char -> \u00XX
                    static const char* hex = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(hex[(c >> 4) & 0xF]);
                    out.push_back(hex[c & 0xF]);
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out;
}

void TrafficMirror::SendJsonBestEffort(std::string json) {
    EnsureSocketForThread();
    if (tl_fd < 0) return;
    if (json.size() > cfg_.maxBytes) json.resize(cfg_.maxBytes);
    ::sendto(tl_fd,
             json.data(),
             json.size(),
             MSG_DONTWAIT,
             reinterpret_cast<const sockaddr*>(&tl_dst),
             sizeof(tl_dst));
}

void TrafficMirror::MirrorRequestHttp1(const std::string& clientIp,
                                       const std::string& backendIpPort,
                                       const HttpRequest& req,
                                       const std::string& bodyNorm) {
    if (!ShouldSample()) return;
    std::string body;
    if (cfg_.includeReqBody) {
        body = bodyNorm;
        if (body.size() > cfg_.maxBodyBytes) body.resize(cfg_.maxBodyBytes);
    }

    std::string json;
    json.reserve(256 + body.size());
    json += "{";
    json += "\"ts_ms\":" + std::to_string(NowUnixMs()) + ",";
    json += "\"event\":\"request\",";
    json += "\"proto\":\"http1\",";
    json += "\"client_ip\":\"" + JsonEscape(clientIp) + "\",";
    json += "\"backend\":\"" + JsonEscape(backendIpPort) + "\",";
    json += "\"method\":\"" + std::string(req.methodString()) + "\",";
    json += "\"path\":\"" + JsonEscape(req.path() + req.query()) + "\"";
    if (cfg_.includeReqBody) {
        json += ",\"req_body\":\"" + JsonEscape(body) + "\"";
    }
    json += "}\n";
    SendJsonBestEffort(std::move(json));
}

void TrafficMirror::MirrorResponseHttp1(const std::string& clientIp,
                                       const std::string& backendIpPort,
                                       const std::string& method,
                                       const std::string& path,
                                       int statusCode,
                                       double rtMs,
                                       const std::string* bodyOpt) {
    if (!ShouldSample()) return;
    std::string body;
    if (cfg_.includeRespBody && bodyOpt) {
        body = *bodyOpt;
        if (body.size() > cfg_.maxBodyBytes) body.resize(cfg_.maxBodyBytes);
    }

    std::string json;
    json.reserve(256 + body.size());
    json += "{";
    json += "\"ts_ms\":" + std::to_string(NowUnixMs()) + ",";
    json += "\"event\":\"response\",";
    json += "\"proto\":\"http1\",";
    json += "\"client_ip\":\"" + JsonEscape(clientIp) + "\",";
    json += "\"backend\":\"" + JsonEscape(backendIpPort) + "\",";
    json += "\"method\":\"" + JsonEscape(method) + "\",";
    json += "\"path\":\"" + JsonEscape(path) + "\",";
    json += "\"status\":" + std::to_string(statusCode) + ",";
    json += "\"rt_ms\":" + std::to_string(rtMs);
    if (cfg_.includeRespBody && bodyOpt) {
        json += ",\"resp_body\":\"" + JsonEscape(body) + "\"";
    }
    json += "}\n";
    SendJsonBestEffort(std::move(json));
}

void TrafficMirror::MirrorRequestHttp2(const std::string& clientIp,
                                       const std::string& backendIpPort,
                                       uint32_t streamId,
                                       const std::string& method,
                                       const std::string& path,
                                       const std::vector<Hpack::Header>&,
                                       const std::string& bodyNorm) {
    if (!ShouldSample()) return;
    std::string body;
    if (cfg_.includeReqBody) {
        body = bodyNorm;
        if (body.size() > cfg_.maxBodyBytes) body.resize(cfg_.maxBodyBytes);
    }

    std::string json;
    json.reserve(256 + body.size());
    json += "{";
    json += "\"ts_ms\":" + std::to_string(NowUnixMs()) + ",";
    json += "\"event\":\"request\",";
    json += "\"proto\":\"http2\",";
    json += "\"stream_id\":" + std::to_string(streamId) + ",";
    json += "\"client_ip\":\"" + JsonEscape(clientIp) + "\",";
    json += "\"backend\":\"" + JsonEscape(backendIpPort) + "\",";
    json += "\"method\":\"" + JsonEscape(method) + "\",";
    json += "\"path\":\"" + JsonEscape(path) + "\"";
    if (cfg_.includeReqBody) {
        json += ",\"req_body\":\"" + JsonEscape(body) + "\"";
    }
    json += "}\n";
    SendJsonBestEffort(std::move(json));
}

void TrafficMirror::MirrorResponseHttp2(const std::string& clientIp,
                                       const std::string& backendIpPort,
                                       uint32_t streamId,
                                       const std::string& method,
                                       const std::string& path,
                                       int statusCode,
                                       double rtMs,
                                       const std::string* bodyOpt) {
    if (!ShouldSample()) return;
    std::string body;
    if (cfg_.includeRespBody && bodyOpt) {
        body = *bodyOpt;
        if (body.size() > cfg_.maxBodyBytes) body.resize(cfg_.maxBodyBytes);
    }

    std::string json;
    json.reserve(256 + body.size());
    json += "{";
    json += "\"ts_ms\":" + std::to_string(NowUnixMs()) + ",";
    json += "\"event\":\"response\",";
    json += "\"proto\":\"http2\",";
    json += "\"stream_id\":" + std::to_string(streamId) + ",";
    json += "\"client_ip\":\"" + JsonEscape(clientIp) + "\",";
    json += "\"backend\":\"" + JsonEscape(backendIpPort) + "\",";
    json += "\"method\":\"" + JsonEscape(method) + "\",";
    json += "\"path\":\"" + JsonEscape(path) + "\",";
    json += "\"status\":" + std::to_string(statusCode) + ",";
    json += "\"rt_ms\":" + std::to_string(rtMs);
    if (cfg_.includeRespBody && bodyOpt) {
        json += ",\"resp_body\":\"" + JsonEscape(body) + "\"";
    }
    json += "}\n";
    SendJsonBestEffort(std::move(json));
}

} // namespace protocol
} // namespace proxy

