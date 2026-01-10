#include "proxy/protocol/ProtobufLite.h"

namespace proxy {
namespace protocol {

bool ProtobufLite::EncodeVarint(uint64_t v, std::string* out) {
    if (!out) return false;
    while (v >= 0x80) {
        out->push_back(static_cast<char>((v & 0x7Fu) | 0x80u));
        v >>= 7;
    }
    out->push_back(static_cast<char>(v & 0x7Fu));
    return true;
}

bool ProtobufLite::DecodeVarint(const uint8_t* data, size_t len, uint64_t* v, size_t* consumed) {
    if (!data || !v || !consumed) return false;
    uint64_t out = 0;
    uint32_t shift = 0;
    size_t off = 0;
    while (off < len && shift <= 63) {
        const uint8_t b = data[off++];
        out |= static_cast<uint64_t>(b & 0x7Fu) << shift;
        if ((b & 0x80u) == 0) {
            *v = out;
            *consumed = off;
            return true;
        }
        shift += 7;
    }
    return false;
}

bool ProtobufLite::EncodeStringField1(const std::string& s, std::string* outMessageBytes) {
    if (!outMessageBytes) return false;
    outMessageBytes->clear();
    // tag = (field_number << 3) | wire_type = (1<<3)|2 = 0x0A
    outMessageBytes->push_back(static_cast<char>(0x0A));
    if (!EncodeVarint(static_cast<uint64_t>(s.size()), outMessageBytes)) return false;
    outMessageBytes->append(s);
    return true;
}

static bool SkipField(uint32_t wireType, const uint8_t* data, size_t len, size_t* consumed) {
    if (!consumed) return false;
    *consumed = 0;
    if (wireType == 0) { // varint
        uint64_t tmp = 0;
        size_t used = 0;
        if (!ProtobufLite::DecodeVarint(data, len, &tmp, &used)) return false;
        *consumed = used;
        return true;
    }
    if (wireType == 2) { // length-delimited
        uint64_t n = 0;
        size_t used = 0;
        if (!ProtobufLite::DecodeVarint(data, len, &n, &used)) return false;
        if (used + n > len) return false;
        *consumed = used + static_cast<size_t>(n);
        return true;
    }
    return false;
}

bool ProtobufLite::DecodeStringField1(const uint8_t* data, size_t len, std::string* out) {
    if (!out) return false;
    out->clear();
    size_t off = 0;
    while (off < len) {
        uint64_t key = 0;
        size_t usedKey = 0;
        if (!DecodeVarint(data + off, len - off, &key, &usedKey)) return false;
        off += usedKey;
        const uint32_t field = static_cast<uint32_t>(key >> 3);
        const uint32_t wire = static_cast<uint32_t>(key & 0x7u);

        if (field == 1 && wire == 2) {
            uint64_t n = 0;
            size_t usedLen = 0;
            if (!DecodeVarint(data + off, len - off, &n, &usedLen)) return false;
            off += usedLen;
            if (off + n > len) return false;
            out->assign(reinterpret_cast<const char*>(data + off), reinterpret_cast<const char*>(data + off + n));
            return true;
        }

        size_t skip = 0;
        if (!SkipField(wire, data + off, len - off, &skip)) return false;
        off += skip;
    }
    return true;
}

} // namespace protocol
} // namespace proxy

