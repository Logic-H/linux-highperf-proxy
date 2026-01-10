#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace proxy {
namespace protocol {

static constexpr const char* kHttp2ConnectionPreface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
static constexpr size_t kHttp2ConnectionPrefaceLen = 24;

enum class Http2FrameType : uint8_t {
    kData = 0x0,
    kHeaders = 0x1,
    kPriority = 0x2,
    kRstStream = 0x3,
    kSettings = 0x4,
    kPushPromise = 0x5,
    kPing = 0x6,
    kGoaway = 0x7,
    kWindowUpdate = 0x8,
    kContinuation = 0x9,
};

struct Http2FrameHeader {
    uint32_t length{0};   // 24-bit
    uint8_t type{0};
    uint8_t flags{0};
    uint32_t streamId{0}; // 31-bit
};

inline void Write24(std::vector<uint8_t>* out, uint32_t v) {
    out->push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out->push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out->push_back(static_cast<uint8_t>(v & 0xFF));
}

inline void Write32(std::vector<uint8_t>* out, uint32_t v) {
    out->push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    out->push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out->push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out->push_back(static_cast<uint8_t>(v & 0xFF));
}

inline void WriteFrame(std::vector<uint8_t>* out, const Http2FrameHeader& h, const uint8_t* payload, size_t payloadLen) {
    Write24(out, h.length);
    out->push_back(h.type);
    out->push_back(h.flags);
    const uint32_t sid = (h.streamId & 0x7FFFFFFF);
    Write32(out, sid);
    if (payloadLen && payload) out->insert(out->end(), payload, payload + payloadLen);
}

} // namespace protocol
} // namespace proxy

