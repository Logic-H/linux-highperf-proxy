#pragma once

#include "proxy/common/noncopyable.h"
#include "proxy/protocol/Http2Common.h"
#include "proxy/protocol/Hpack.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace proxy {
namespace protocol {

// Minimal HTTP/2 (h2c prior-knowledge) connection handler.
// - Handles SETTINGS/PING.
// - Parses HEADERS(+CONTINUATION) and DATA to build full requests per stream.
// - Uses HPACK decoder for request headers and minimal encoder for response headers.
class Http2Connection : proxy::common::noncopyable {
public:
    struct Request {
        uint32_t streamId{0};
        std::string method;
        std::string path;      // includes query
        std::string authority; // :authority
        std::string scheme;    // :scheme
        std::vector<Hpack::Header> headers;
        std::string body;
    };

    using SendCallback = std::function<void(const void* data, size_t len)>;
    using RequestCallback = std::function<void(const Request& req)>;

    Http2Connection() = default;

    void Reset();
    void SetSendCallback(SendCallback cb) { sendCb_ = std::move(cb); }
    void SetRequestCallback(RequestCallback cb) { reqCb_ = std::move(cb); }

    // Feed raw TCP bytes.
    void OnData(const uint8_t* data, size_t len);

    // Send a simple response: HEADERS(:status + extra) + DATA.
    void SendResponse(uint32_t streamId, int status, const std::vector<Hpack::Header>& headers, const std::string& body, bool endStream = true);

    // Lower-level helpers (for streaming protocols like gRPC):
    // - Send response headers (includes :status). Does not auto-add content-length.
    void SendHeaders(uint32_t streamId, int status, const std::vector<Hpack::Header>& headers, bool endStream = false);
    // - Send a DATA frame. Caller decides endStream.
    void SendData(uint32_t streamId, const std::string& data, bool endStream = false);
    // - Send trailing headers (END_STREAM).
    void SendTrailers(uint32_t streamId, const std::vector<Hpack::Header>& headers);

private:
    struct StreamState {
        std::vector<uint8_t> headerBlock;
        bool gotEndHeaders{false};
        bool gotEndStream{false};
        std::vector<Hpack::Header> headers;
        std::string method;
        std::string path;
        std::string authority;
        std::string scheme;
        std::string body;
    };

    void sendSettings();
    void sendSettingsAck();
    void sendPingAck(const uint8_t* opaque8);

    void parseFrames();
    bool parseOneFrame(Http2FrameHeader* out, std::vector<uint8_t>* payload);
    void handleFrame(const Http2FrameHeader& h, const uint8_t* payload, size_t payloadLen);

    void handleSettings(const Http2FrameHeader& h, const uint8_t* payload, size_t payloadLen);
    void handleHeaders(const Http2FrameHeader& h, const uint8_t* payload, size_t payloadLen);
    void handleContinuation(const Http2FrameHeader& h, const uint8_t* payload, size_t payloadLen);
    void handleData(const Http2FrameHeader& h, const uint8_t* payload, size_t payloadLen);
    void handlePing(const Http2FrameHeader& h, const uint8_t* payload, size_t payloadLen);

    void tryEmitRequest(uint32_t streamId);

    bool prefaceDone_{false};
    std::vector<uint8_t> in_;
    Hpack hpack_;
    std::map<uint32_t, StreamState> streams_;

    SendCallback sendCb_;
    RequestCallback reqCb_;
};

} // namespace protocol
} // namespace proxy
