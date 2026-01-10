#include "proxy/protocol/Http2Connection.h"

#include <algorithm>
#include <cstring>

namespace proxy {
namespace protocol {

static bool isPrefacePrefix(const uint8_t* data, size_t len) {
    if (len > kHttp2ConnectionPrefaceLen) len = kHttp2ConnectionPrefaceLen;
    return std::memcmp(data, kHttp2ConnectionPreface, len) == 0;
}

void Http2Connection::Reset() {
    prefaceDone_ = false;
    in_.clear();
    hpack_.Reset();
    streams_.clear();
}

void Http2Connection::OnData(const uint8_t* data, size_t len) {
    if (!data || len == 0) return;
    in_.insert(in_.end(), data, data + len);

    if (!prefaceDone_) {
        if (in_.size() < kHttp2ConnectionPrefaceLen) {
            // wait for more, but if it no longer matches preface prefix, do nothing.
            return;
        }
        if (!isPrefacePrefix(in_.data(), kHttp2ConnectionPrefaceLen)) {
            // not HTTP/2
            in_.clear();
            return;
        }
        in_.erase(in_.begin(), in_.begin() + kHttp2ConnectionPrefaceLen);
        prefaceDone_ = true;
        sendSettings();
    }

    parseFrames();
}

bool Http2Connection::parseOneFrame(Http2FrameHeader* out, std::vector<uint8_t>* payload) {
    if (in_.size() < 9) return false;
    const uint32_t len = (static_cast<uint32_t>(in_[0]) << 16) | (static_cast<uint32_t>(in_[1]) << 8) | static_cast<uint32_t>(in_[2]);
    if (in_.size() < 9 + len) return false;

    Http2FrameHeader h;
    h.length = len;
    h.type = in_[3];
    h.flags = in_[4];
    uint32_t sid = (static_cast<uint32_t>(in_[5]) << 24) | (static_cast<uint32_t>(in_[6]) << 16) | (static_cast<uint32_t>(in_[7]) << 8) |
                   static_cast<uint32_t>(in_[8]);
    h.streamId = sid & 0x7FFFFFFF;

    if (payload) {
        payload->assign(in_.begin() + 9, in_.begin() + 9 + len);
    }
    in_.erase(in_.begin(), in_.begin() + 9 + len);
    if (out) *out = h;
    return true;
}

void Http2Connection::parseFrames() {
    Http2FrameHeader h;
    std::vector<uint8_t> p;
    while (parseOneFrame(&h, &p)) {
        handleFrame(h, p.data(), p.size());
        p.clear();
    }
}

void Http2Connection::handleFrame(const Http2FrameHeader& h, const uint8_t* payload, size_t payloadLen) {
    switch (static_cast<Http2FrameType>(h.type)) {
        case Http2FrameType::kSettings:
            handleSettings(h, payload, payloadLen);
            break;
        case Http2FrameType::kHeaders:
            handleHeaders(h, payload, payloadLen);
            break;
        case Http2FrameType::kContinuation:
            handleContinuation(h, payload, payloadLen);
            break;
        case Http2FrameType::kData:
            handleData(h, payload, payloadLen);
            break;
        case Http2FrameType::kPing:
            handlePing(h, payload, payloadLen);
            break;
        default:
            break;
    }
}

void Http2Connection::handleSettings(const Http2FrameHeader& h, const uint8_t* payload, size_t payloadLen) {
    if (h.streamId != 0) return;
    const bool ack = (h.flags & 0x1u) != 0;
    if (ack) return;

    // Parse settings: each 6 bytes (id(16), value(32))
    size_t off = 0;
    while (off + 6 <= payloadLen) {
        const uint16_t id = static_cast<uint16_t>((payload[off] << 8) | payload[off + 1]);
        const uint32_t val = (static_cast<uint32_t>(payload[off + 2]) << 24) | (static_cast<uint32_t>(payload[off + 3]) << 16) |
                             (static_cast<uint32_t>(payload[off + 4]) << 8) | static_cast<uint32_t>(payload[off + 5]);
        // HEADER_TABLE_SIZE
        if (id == 0x1) {
            hpack_.SetMaxDynamicTableSize(val);
        }
        off += 6;
    }

    sendSettingsAck();
}

void Http2Connection::handlePing(const Http2FrameHeader& h, const uint8_t* payload, size_t payloadLen) {
    if (h.streamId != 0) return;
    const bool ack = (h.flags & 0x1u) != 0;
    if (ack) return;
    if (payloadLen != 8) return;
    sendPingAck(payload);
}

void Http2Connection::handleHeaders(const Http2FrameHeader& h, const uint8_t* payload, size_t payloadLen) {
    if (h.streamId == 0) return;
    auto& st = streams_[h.streamId];

    size_t off = 0;
    if (h.flags & 0x8u) {
        // PADDED
        if (payloadLen < 1) return;
        const uint8_t pad = payload[0];
        off = 1;
        if (payloadLen < off + pad) return;
        payloadLen -= pad;
    }
    if (h.flags & 0x20u) {
        // PRIORITY (5 bytes)
        if (payloadLen < off + 5) return;
        off += 5;
    }

    const bool endHeaders = (h.flags & 0x4u) != 0;
    const bool endStream = (h.flags & 0x1u) != 0;
    if (endStream) st.gotEndStream = true;

    st.headerBlock.insert(st.headerBlock.end(), payload + off, payload + payloadLen);
    if (endHeaders) {
        st.gotEndHeaders = true;
        std::vector<Hpack::Header> hdrs;
        if (!hpack_.Decode(st.headerBlock.data(), st.headerBlock.size(), &hdrs)) {
            return;
        }
        st.headers = hdrs;
        for (const auto& hh : hdrs) {
            if (hh.name == ":method") st.method = hh.value;
            else if (hh.name == ":path") st.path = hh.value;
            else if (hh.name == ":authority") st.authority = hh.value;
            else if (hh.name == ":scheme") st.scheme = hh.value;
        }
        st.headerBlock.clear();
        tryEmitRequest(h.streamId);
    }
}

void Http2Connection::handleContinuation(const Http2FrameHeader& h, const uint8_t* payload, size_t payloadLen) {
    if (h.streamId == 0) return;
    auto it = streams_.find(h.streamId);
    if (it == streams_.end()) return;
    auto& st = it->second;
    st.headerBlock.insert(st.headerBlock.end(), payload, payload + payloadLen);
    if (h.flags & 0x4u) {
        st.gotEndHeaders = true;
        std::vector<Hpack::Header> hdrs;
        if (!hpack_.Decode(st.headerBlock.data(), st.headerBlock.size(), &hdrs)) {
            return;
        }
        st.headers = hdrs;
        for (const auto& hh : hdrs) {
            if (hh.name == ":method") st.method = hh.value;
            else if (hh.name == ":path") st.path = hh.value;
            else if (hh.name == ":authority") st.authority = hh.value;
            else if (hh.name == ":scheme") st.scheme = hh.value;
        }
        st.headerBlock.clear();
        tryEmitRequest(h.streamId);
    }
}

void Http2Connection::handleData(const Http2FrameHeader& h, const uint8_t* payload, size_t payloadLen) {
    if (h.streamId == 0) return;
    auto& st = streams_[h.streamId];
    size_t off = 0;
    if (h.flags & 0x8u) {
        if (payloadLen < 1) return;
        const uint8_t pad = payload[0];
        off = 1;
        if (payloadLen < off + pad) return;
        payloadLen -= pad;
    }
    const bool endStream = (h.flags & 0x1u) != 0;
    if (payloadLen > off) {
        st.body.append(reinterpret_cast<const char*>(payload + off), reinterpret_cast<const char*>(payload + payloadLen));
    }
    if (endStream) st.gotEndStream = true;
    tryEmitRequest(h.streamId);
}

void Http2Connection::tryEmitRequest(uint32_t streamId) {
    auto it = streams_.find(streamId);
    if (it == streams_.end()) return;
    auto& st = it->second;
    if (!st.gotEndHeaders) return;
    if (!st.gotEndStream) return;
    if (!reqCb_) {
        streams_.erase(it);
        return;
    }
    Request r;
    r.streamId = streamId;
    r.method = st.method.empty() ? "GET" : st.method;
    r.path = st.path.empty() ? "/" : st.path;
    r.authority = st.authority;
    r.scheme = st.scheme;
    r.headers = st.headers;
    r.body = st.body;
    streams_.erase(it);
    reqCb_(r);
}

void Http2Connection::sendSettings() {
    if (!sendCb_) return;
    std::vector<uint8_t> out;
    Http2FrameHeader h;
    h.length = 0;
    h.type = static_cast<uint8_t>(Http2FrameType::kSettings);
    h.flags = 0;
    h.streamId = 0;
    WriteFrame(&out, h, nullptr, 0);
    sendCb_(out.data(), out.size());
}

void Http2Connection::sendSettingsAck() {
    if (!sendCb_) return;
    std::vector<uint8_t> out;
    Http2FrameHeader h;
    h.length = 0;
    h.type = static_cast<uint8_t>(Http2FrameType::kSettings);
    h.flags = 0x1;
    h.streamId = 0;
    WriteFrame(&out, h, nullptr, 0);
    sendCb_(out.data(), out.size());
}

void Http2Connection::sendPingAck(const uint8_t* opaque8) {
    if (!sendCb_) return;
    std::vector<uint8_t> out;
    Http2FrameHeader h;
    h.length = 8;
    h.type = static_cast<uint8_t>(Http2FrameType::kPing);
    h.flags = 0x1;
    h.streamId = 0;
    WriteFrame(&out, h, opaque8, 8);
    sendCb_(out.data(), out.size());
}

void Http2Connection::SendResponse(uint32_t streamId,
                                   int status,
                                   const std::vector<Hpack::Header>& headers,
                                   const std::string& body,
                                   bool endStream) {
    if (!sendCb_ || streamId == 0) return;
    std::vector<Hpack::Header> hs;
    hs.reserve(3 + headers.size());
    hs.push_back({":status", std::to_string(status)});
    hs.push_back({"content-length", std::to_string(body.size())});
    for (const auto& h : headers) hs.push_back(h);

    const std::vector<uint8_t> block = hpack_.EncodeNoIndex(hs);

    std::vector<uint8_t> out;
    // HEADERS
    {
        Http2FrameHeader h;
        h.length = static_cast<uint32_t>(block.size());
        h.type = static_cast<uint8_t>(Http2FrameType::kHeaders);
        h.flags = 0x4; // END_HEADERS
        if (endStream && body.empty()) h.flags |= 0x1; // END_STREAM
        h.streamId = streamId;
        WriteFrame(&out, h, block.data(), block.size());
    }
    // DATA
    if (!body.empty()) {
        Http2FrameHeader h;
        h.length = static_cast<uint32_t>(body.size());
        h.type = static_cast<uint8_t>(Http2FrameType::kData);
        h.flags = endStream ? 0x1 : 0;
        h.streamId = streamId;
        WriteFrame(&out, h, reinterpret_cast<const uint8_t*>(body.data()), body.size());
    }
    sendCb_(out.data(), out.size());
}

void Http2Connection::SendHeaders(uint32_t streamId, int status, const std::vector<Hpack::Header>& headers, bool endStream) {
    if (!sendCb_ || streamId == 0) return;
    std::vector<Hpack::Header> hs;
    hs.reserve(1 + headers.size());
    hs.push_back({":status", std::to_string(status)});
    for (const auto& h : headers) hs.push_back(h);

    const std::vector<uint8_t> block = hpack_.EncodeNoIndex(hs);
    std::vector<uint8_t> out;
    Http2FrameHeader h;
    h.length = static_cast<uint32_t>(block.size());
    h.type = static_cast<uint8_t>(Http2FrameType::kHeaders);
    h.flags = 0x4; // END_HEADERS
    if (endStream) h.flags |= 0x1; // END_STREAM
    h.streamId = streamId;
    WriteFrame(&out, h, block.data(), block.size());
    sendCb_(out.data(), out.size());
}

void Http2Connection::SendData(uint32_t streamId, const std::string& data, bool endStream) {
    if (!sendCb_ || streamId == 0) return;
    Http2FrameHeader h;
    h.length = static_cast<uint32_t>(data.size());
    h.type = static_cast<uint8_t>(Http2FrameType::kData);
    h.flags = endStream ? 0x1 : 0;
    h.streamId = streamId;
    std::vector<uint8_t> out;
    WriteFrame(&out, h, reinterpret_cast<const uint8_t*>(data.data()), data.size());
    sendCb_(out.data(), out.size());
}

void Http2Connection::SendTrailers(uint32_t streamId, const std::vector<Hpack::Header>& headers) {
    if (!sendCb_ || streamId == 0) return;
    const std::vector<uint8_t> block = hpack_.EncodeNoIndex(headers);
    Http2FrameHeader h;
    h.length = static_cast<uint32_t>(block.size());
    h.type = static_cast<uint8_t>(Http2FrameType::kHeaders);
    h.flags = 0x4 | 0x1; // END_HEADERS | END_STREAM
    h.streamId = streamId;
    std::vector<uint8_t> out;
    WriteFrame(&out, h, block.data(), block.size());
    sendCb_(out.data(), out.size());
}

} // namespace protocol
} // namespace proxy
