#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace proxy {
namespace protocol {

class Compression {
public:
    enum class Encoding {
        kIdentity,
        kGzip,
        kDeflate,
        kUnknown,
    };

    static Encoding ParseContentEncoding(const std::string& v);

    // Decompress whole buffer.
    static bool Decompress(Encoding enc, const uint8_t* data, size_t len, std::string* out);
    static bool Decompress(Encoding enc, const std::string& in, std::string* out);

    // Compress whole buffer.
    static bool Compress(Encoding enc, const uint8_t* data, size_t len, std::string* out);
    static bool Compress(Encoding enc, const std::string& in, std::string* out);
};

} // namespace protocol
} // namespace proxy

