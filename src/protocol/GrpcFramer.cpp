#include "proxy/protocol/GrpcFramer.h"

namespace proxy {
namespace protocol {

bool GrpcFramer::EncodeMessage(const std::string& messageBytes, std::string* outFrame) {
    if (!outFrame) return false;
    outFrame->clear();
    outFrame->push_back(static_cast<char>(0x00)); // uncompressed
    const uint32_t n = static_cast<uint32_t>(messageBytes.size());
    outFrame->push_back(static_cast<char>((n >> 24) & 0xFF));
    outFrame->push_back(static_cast<char>((n >> 16) & 0xFF));
    outFrame->push_back(static_cast<char>((n >> 8) & 0xFF));
    outFrame->push_back(static_cast<char>(n & 0xFF));
    outFrame->append(messageBytes);
    return true;
}

bool GrpcFramer::DecodeMessages(const uint8_t* data, size_t len, std::vector<std::string>* outMessages) {
    if (!outMessages) return false;
    outMessages->clear();
    if (!data && len != 0) return false;
    size_t off = 0;
    while (off < len) {
        if (len - off < 5) return false;
        const uint8_t compressed = data[off];
        const uint32_t n = (static_cast<uint32_t>(data[off + 1]) << 24) | (static_cast<uint32_t>(data[off + 2]) << 16) |
                           (static_cast<uint32_t>(data[off + 3]) << 8) | static_cast<uint32_t>(data[off + 4]);
        off += 5;
        if (compressed != 0) return false;
        if (off + n > len) return false;
        outMessages->emplace_back(reinterpret_cast<const char*>(data + off), reinterpret_cast<const char*>(data + off + n));
        off += n;
    }
    return true;
}

} // namespace protocol
} // namespace proxy

