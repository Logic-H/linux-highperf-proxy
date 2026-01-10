#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace proxy {
namespace protocol {

// Minimal protobuf helpers for this assignment:
// - Supports varint decode/encode.
// - Supports encoding/decoding a message with field #1 as a length-delimited string (wire type 2).
class ProtobufLite {
public:
    static bool EncodeVarint(uint64_t v, std::string* out);
    static bool DecodeVarint(const uint8_t* data, size_t len, uint64_t* v, size_t* consumed);

    // Encode message: field 1 (string) only.
    static bool EncodeStringField1(const std::string& s, std::string* outMessageBytes);

    // Decode message: extracts field 1 (string). Ignores unknown fields.
    static bool DecodeStringField1(const uint8_t* data, size_t len, std::string* out);
};

} // namespace protocol
} // namespace proxy

