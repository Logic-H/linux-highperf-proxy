#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace proxy {
namespace protocol {

// Minimal gRPC message framer:
// - Supports uncompressed messages only (compressed flag must be 0).
// - Format: 1 byte compressed flag + 4 bytes big-endian length + message bytes.
class GrpcFramer {
public:
    static bool EncodeMessage(const std::string& messageBytes, std::string* outFrame);
    static bool DecodeMessages(const uint8_t* data, size_t len, std::vector<std::string>* outMessages);
};

} // namespace protocol
} // namespace proxy

