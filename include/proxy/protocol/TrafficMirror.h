#pragma once

#include "proxy/protocol/HttpRequest.h"
#include "proxy/protocol/Hpack.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace proxy {
namespace protocol {

// Traffic mirroring (fire-and-forget):
// - Sends a copy of request/response metadata to an external monitoring system.
// - Designed to not impact the main proxy path: best-effort, non-blocking, drops on error.
// - Current implementation uses UDP datagrams (one JSON per event).
class TrafficMirror {
public:
    struct Config {
        bool enabled{false};
        std::string udpHost{"127.0.0.1"};
        uint16_t udpPort{0};
        double sampleRate{1.0};       // 0..1
        size_t maxBytes{4096};         // max datagram payload
        size_t maxBodyBytes{1024};     // max bytes captured from body
        bool includeReqBody{true};
        bool includeRespBody{false};
    };

    void Configure(const Config& cfg);
    const Config& config() const { return cfg_; }

    void MirrorRequestHttp1(const std::string& clientIp,
                            const std::string& backendIpPort,
                            const HttpRequest& req,
                            const std::string& bodyNorm);

    void MirrorResponseHttp1(const std::string& clientIp,
                             const std::string& backendIpPort,
                             const std::string& method,
                             const std::string& path,
                             int statusCode,
                             double rtMs,
                             const std::string* bodyOpt);

    void MirrorRequestHttp2(const std::string& clientIp,
                            const std::string& backendIpPort,
                            uint32_t streamId,
                            const std::string& method,
                            const std::string& path,
                            const std::vector<Hpack::Header>& headers,
                            const std::string& bodyNorm);

    void MirrorResponseHttp2(const std::string& clientIp,
                             const std::string& backendIpPort,
                             uint32_t streamId,
                             const std::string& method,
                             const std::string& path,
                             int statusCode,
                             double rtMs,
                             const std::string* bodyOpt);

private:
    static std::string JsonEscape(const std::string& s);
    static uint64_t NowUnixMs();
    bool ShouldSample();
    void SendJsonBestEffort(std::string json);
    void EnsureSocketForThread();

    Config cfg_{};

    std::atomic<uint32_t> rng_{0x12345678u};
    std::atomic<uint32_t> cfgVersion_{1};
};

} // namespace protocol
} // namespace proxy

