#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace proxy {
namespace protocol {

// Minimal HPACK decoder/encoder:
// - Supports static table + dynamic table.
// - Supports Huffman decoding for string literals.
// - Encoder uses "Literal Header Field without Indexing" (no Huffman) for simplicity.
class Hpack {
public:
    struct Entry {
        std::string name;
        std::string value;
        size_t size() const { return name.size() + value.size() + 32; }
    };

    struct Header {
        std::string name;
        std::string value;
    };

    Hpack();
    void Reset();
    void SetMaxDynamicTableSize(uint32_t bytes);

    // Decode a header block fragment into headers.
    // Returns false on parse error.
    bool Decode(const uint8_t* data, size_t len, std::vector<Header>* outHeaders);

    // Encode response headers (no indexing, no huffman).
    std::vector<uint8_t> EncodeNoIndex(const std::vector<Header>& headers);

private:
    static bool DecodeInteger(const uint8_t* data, size_t len, uint8_t prefixBits, uint32_t* outValue, size_t* outConsumed);
    static bool DecodeString(const uint8_t* data, size_t len, std::string* out, size_t* outConsumed);
    static bool HuffmanDecode(const uint8_t* data, size_t len, std::string* out);

    static void EncodeInteger(std::vector<uint8_t>* out, uint32_t value, uint8_t prefixBits, uint8_t firstByteMask);
    static void EncodeStringRaw(std::vector<uint8_t>* out, const std::string& s); // no huffman

    const Entry* GetByIndex(uint32_t idx) const;
    void AddDynamic(const std::string& name, const std::string& value);
    void UpdateMaxSize(uint32_t newSize);

    std::vector<Entry> dynamic_; // newest first
    size_t dynamicSize_{0};
    size_t maxSize_{4096};
};

} // namespace protocol
} // namespace proxy
