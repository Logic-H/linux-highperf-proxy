#include "proxy/protocol/Hpack.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace proxy {
namespace protocol {

static const Hpack::Entry kStaticTable[] = {
    {":authority", ""},                  // 1
    {":method", "GET"},                  // 2
    {":method", "POST"},                 // 3
    {":path", "/"},                      // 4
    {":path", "/index.html"},            // 5
    {":scheme", "http"},                 // 6
    {":scheme", "https"},                // 7
    {":status", "200"},                  // 8
    {":status", "204"},                  // 9
    {":status", "206"},                  // 10
    {":status", "304"},                  // 11
    {":status", "400"},                  // 12
    {":status", "404"},                  // 13
    {":status", "500"},                  // 14
    {"accept-charset", ""},              // 15
    {"accept-encoding", "gzip, deflate"},// 16
    {"accept-language", ""},             // 17
    {"accept-ranges", ""},               // 18
    {"accept", ""},                      // 19
    {"access-control-allow-origin", ""}, // 20
    {"age", ""},                         // 21
    {"allow", ""},                       // 22
    {"authorization", ""},               // 23
    {"cache-control", ""},               // 24
    {"content-disposition", ""},         // 25
    {"content-encoding", ""},            // 26
    {"content-language", ""},            // 27
    {"content-length", ""},              // 28
    {"content-location", ""},            // 29
    {"content-range", ""},               // 30
    {"content-type", ""},                // 31
    {"cookie", ""},                      // 32
    {"date", ""},                        // 33
    {"etag", ""},                        // 34
    {"expect", ""},                      // 35
    {"expires", ""},                     // 36
    {"from", ""},                        // 37
    {"host", ""},                        // 38
    {"if-match", ""},                    // 39
    {"if-modified-since", ""},           // 40
    {"if-none-match", ""},               // 41
    {"if-range", ""},                    // 42
    {"if-unmodified-since", ""},         // 43
    {"last-modified", ""},               // 44
    {"link", ""},                        // 45
    {"location", ""},                    // 46
    {"max-forwards", ""},                // 47
    {"proxy-authenticate", ""},          // 48
    {"proxy-authorization", ""},         // 49
    {"range", ""},                       // 50
    {"referer", ""},                     // 51
    {"refresh", ""},                     // 52
    {"retry-after", ""},                 // 53
    {"server", ""},                      // 54
    {"set-cookie", ""},                  // 55
    {"strict-transport-security", ""},   // 56
    {"transfer-encoding", ""},           // 57
    {"user-agent", ""},                  // 58
    {"vary", ""},                        // 59
    {"via", ""},                         // 60
    {"www-authenticate", ""},            // 61
};

static constexpr size_t kStaticTableSize = sizeof(kStaticTable) / sizeof(kStaticTable[0]);

Hpack::Hpack() { Reset(); }

void Hpack::Reset() {
    dynamic_.clear();
    dynamicSize_ = 0;
    maxSize_ = 4096;
}

void Hpack::SetMaxDynamicTableSize(uint32_t bytes) {
    UpdateMaxSize(bytes);
}

const Hpack::Entry* Hpack::GetByIndex(uint32_t idx) const {
    if (idx == 0) return nullptr;
    if (idx <= kStaticTableSize) {
        return &kStaticTable[idx - 1];
    }
    const uint32_t d = idx - static_cast<uint32_t>(kStaticTableSize) - 1;
    if (d < dynamic_.size()) {
        return &dynamic_[d];
    }
    return nullptr;
}

void Hpack::UpdateMaxSize(uint32_t newSize) {
    maxSize_ = newSize;
    while (dynamicSize_ > maxSize_ && !dynamic_.empty()) {
        dynamicSize_ -= dynamic_.back().size();
        dynamic_.pop_back();
    }
}

void Hpack::AddDynamic(const std::string& name, const std::string& value) {
    Entry e{name, value};
    const size_t sz = e.size();
    if (sz > maxSize_) {
        dynamic_.clear();
        dynamicSize_ = 0;
        return;
    }
    dynamic_.insert(dynamic_.begin(), std::move(e));
    dynamicSize_ += sz;
    while (dynamicSize_ > maxSize_ && !dynamic_.empty()) {
        dynamicSize_ -= dynamic_.back().size();
        dynamic_.pop_back();
    }
}

bool Hpack::DecodeInteger(const uint8_t* data, size_t len, uint8_t prefixBits, uint32_t* outValue, size_t* outConsumed) {
    if (!data || len == 0 || !outValue || !outConsumed) return false;
    const uint8_t prefixMax = static_cast<uint8_t>((1u << prefixBits) - 1u);
    uint32_t val = static_cast<uint32_t>(data[0] & prefixMax);
    size_t off = 1;
    if (val == prefixMax) {
        uint32_t m = 0;
        while (off < len) {
            const uint8_t b = data[off++];
            val += static_cast<uint32_t>(b & 0x7Fu) << m;
            if ((b & 0x80u) == 0) break;
            m += 7;
            if (m > 28) return false;
        }
        if (off > len) return false;
    }
    *outValue = val;
    *outConsumed = off;
    return true;
}

// HPACK Huffman decode table (RFC 7541 Appendix B).
// We store codes left-aligned in 32-bit words (MSB-first) to simplify bit walking.
struct HuffSym {
    uint8_t bits;
    uint32_t code_msb;
};

// Index 0..255 are literal bytes, 256 is EOS (must not appear in a valid string).
static constexpr HuffSym kHuffSymTable[257] = {
    {13, 0xffc00000u},     {23, 0xffffb000u},     {28, 0xfffffe20u},     {28, 0xfffffe30u},
    {28, 0xfffffe40u},     {28, 0xfffffe50u},     {28, 0xfffffe60u},     {28, 0xfffffe70u},
    {28, 0xfffffe80u},     {24, 0xffffea00u},     {30, 0xfffffff0u},     {28, 0xfffffe90u},
    {28, 0xfffffea0u},     {30, 0xfffffff4u},     {28, 0xfffffeb0u},     {28, 0xfffffec0u},
    {28, 0xfffffed0u},     {28, 0xfffffee0u},     {28, 0xfffffef0u},     {28, 0xffffff00u},
    {28, 0xffffff10u},     {28, 0xffffff20u},     {30, 0xfffffff8u},     {28, 0xffffff30u},
    {28, 0xffffff40u},     {28, 0xffffff50u},     {28, 0xffffff60u},     {28, 0xffffff70u},
    {28, 0xffffff80u},     {28, 0xffffff90u},     {28, 0xffffffa0u},     {28, 0xffffffb0u},
    {6, 0x50000000u},     {10, 0xfe000000u},     {10, 0xfe400000u},     {12, 0xffa00000u},
    {13, 0xffc80000u},     {6, 0x54000000u},     {8, 0xf8000000u},     {11, 0xff400000u},
    {10, 0xfe800000u},     {10, 0xfec00000u},     {8, 0xf9000000u},     {11, 0xff600000u},
    {8, 0xfa000000u},     {6, 0x58000000u},     {6, 0x5c000000u},     {6, 0x60000000u},
    {5, 0x0u},     {5, 0x8000000u},     {5, 0x10000000u},     {6, 0x64000000u},
    {6, 0x68000000u},     {6, 0x6c000000u},     {6, 0x70000000u},     {6, 0x74000000u},
    {6, 0x78000000u},     {6, 0x7c000000u},     {7, 0xb8000000u},     {8, 0xfb000000u},
    {15, 0xfff80000u},     {6, 0x80000000u},     {12, 0xffb00000u},     {10, 0xff000000u},
    {13, 0xffd00000u},     {6, 0x84000000u},     {7, 0xba000000u},     {7, 0xbc000000u},
    {7, 0xbe000000u},     {7, 0xc0000000u},     {7, 0xc2000000u},     {7, 0xc4000000u},
    {7, 0xc6000000u},     {7, 0xc8000000u},     {7, 0xca000000u},     {7, 0xcc000000u},
    {7, 0xce000000u},     {7, 0xd0000000u},     {7, 0xd2000000u},     {7, 0xd4000000u},
    {7, 0xd6000000u},     {7, 0xd8000000u},     {7, 0xda000000u},     {7, 0xdc000000u},
    {7, 0xde000000u},     {7, 0xe0000000u},     {7, 0xe2000000u},     {7, 0xe4000000u},
    {8, 0xfc000000u},     {7, 0xe6000000u},     {8, 0xfd000000u},     {13, 0xffd80000u},
    {19, 0xfffe0000u},     {13, 0xffe00000u},     {14, 0xfff00000u},     {6, 0x88000000u},
    {15, 0xfffa0000u},     {5, 0x18000000u},     {6, 0x8c000000u},     {5, 0x20000000u},
    {6, 0x90000000u},     {5, 0x28000000u},     {6, 0x94000000u},     {6, 0x98000000u},
    {6, 0x9c000000u},     {5, 0x30000000u},     {7, 0xe8000000u},     {7, 0xea000000u},
    {6, 0xa0000000u},     {6, 0xa4000000u},     {6, 0xa8000000u},     {5, 0x38000000u},
    {6, 0xac000000u},     {7, 0xec000000u},     {6, 0xb0000000u},     {5, 0x40000000u},
    {5, 0x48000000u},     {6, 0xb4000000u},     {7, 0xee000000u},     {7, 0xf0000000u},
    {7, 0xf2000000u},     {7, 0xf4000000u},     {7, 0xf6000000u},     {15, 0xfffc0000u},
    {11, 0xff800000u},     {14, 0xfff40000u},     {13, 0xffe80000u},     {28, 0xffffffc0u},
    {20, 0xfffe6000u},     {22, 0xffff4800u},     {20, 0xfffe7000u},     {20, 0xfffe8000u},
    {22, 0xffff4c00u},     {22, 0xffff5000u},     {22, 0xffff5400u},     {23, 0xffffb200u},
    {22, 0xffff5800u},     {23, 0xffffb400u},     {23, 0xffffb600u},     {23, 0xffffb800u},
    {23, 0xffffba00u},     {23, 0xffffbc00u},     {24, 0xffffeb00u},     {23, 0xffffbe00u},
    {24, 0xffffec00u},     {24, 0xffffed00u},     {22, 0xffff5c00u},     {23, 0xffffc000u},
    {24, 0xffffee00u},     {23, 0xffffc200u},     {23, 0xffffc400u},     {23, 0xffffc600u},
    {23, 0xffffc800u},     {21, 0xfffee000u},     {22, 0xffff6000u},     {23, 0xffffca00u},
    {22, 0xffff6400u},     {23, 0xffffcc00u},     {23, 0xffffce00u},     {24, 0xffffef00u},
    {22, 0xffff6800u},     {21, 0xfffee800u},     {20, 0xfffe9000u},     {22, 0xffff6c00u},
    {22, 0xffff7000u},     {23, 0xffffd000u},     {23, 0xffffd200u},     {21, 0xfffef000u},
    {23, 0xffffd400u},     {22, 0xffff7400u},     {22, 0xffff7800u},     {24, 0xfffff000u},
    {21, 0xfffef800u},     {22, 0xffff7c00u},     {23, 0xffffd600u},     {23, 0xffffd800u},
    {21, 0xffff0000u},     {21, 0xffff0800u},     {22, 0xffff8000u},     {21, 0xffff1000u},
    {23, 0xffffda00u},     {22, 0xffff8400u},     {23, 0xffffdc00u},     {23, 0xffffde00u},
    {20, 0xfffea000u},     {22, 0xffff8800u},     {22, 0xffff8c00u},     {22, 0xffff9000u},
    {23, 0xffffe000u},     {22, 0xffff9400u},     {22, 0xffff9800u},     {23, 0xffffe200u},
    {26, 0xfffff800u},     {26, 0xfffff840u},     {20, 0xfffeb000u},     {19, 0xfffe2000u},
    {22, 0xffff9c00u},     {23, 0xffffe400u},     {22, 0xffffa000u},     {25, 0xfffff600u},
    {26, 0xfffff880u},     {26, 0xfffff8c0u},     {26, 0xfffff900u},     {27, 0xfffffbc0u},
    {27, 0xfffffbe0u},     {26, 0xfffff940u},     {24, 0xfffff100u},     {25, 0xfffff680u},
    {19, 0xfffe4000u},     {21, 0xffff1800u},     {26, 0xfffff980u},     {27, 0xfffffc00u},
    {27, 0xfffffc20u},     {26, 0xfffff9c0u},     {27, 0xfffffc40u},     {24, 0xfffff200u},
    {21, 0xffff2000u},     {21, 0xffff2800u},     {26, 0xfffffa00u},     {26, 0xfffffa40u},
    {28, 0xffffffd0u},     {27, 0xfffffc60u},     {27, 0xfffffc80u},     {27, 0xfffffca0u},
    {20, 0xfffec000u},     {24, 0xfffff300u},     {20, 0xfffed000u},     {21, 0xffff3000u},
    {22, 0xffffa400u},     {21, 0xffff3800u},     {21, 0xffff4000u},     {23, 0xffffe600u},
    {22, 0xffffa800u},     {22, 0xffffac00u},     {25, 0xfffff700u},     {25, 0xfffff780u},
    {24, 0xfffff400u},     {24, 0xfffff500u},     {26, 0xfffffa80u},     {23, 0xffffe800u},
    {26, 0xfffffac0u},     {27, 0xfffffcc0u},     {26, 0xfffffb00u},     {26, 0xfffffb40u},
    {27, 0xfffffce0u},     {27, 0xfffffd00u},     {27, 0xfffffd20u},     {27, 0xfffffd40u},
    {27, 0xfffffd60u},     {28, 0xffffffe0u},     {27, 0xfffffd80u},     {27, 0xfffffda0u},
    {27, 0xfffffdc0u},     {27, 0xfffffde0u},     {27, 0xfffffe00u},     {26, 0xfffffb80u},
    {30, 0xfffffffcu},
};

struct HuffNode {
    int16_t next[2]{-1, -1};
    int16_t sym{-1};
};

static const std::vector<HuffNode>& GetHuffTrie() {
    static std::vector<HuffNode> trie = []() {
        std::vector<HuffNode> t;
        t.reserve(4096);
        t.emplace_back(); // root

        for (int sym = 0; sym < 257; ++sym) {
            const uint8_t bits = kHuffSymTable[sym].bits;
            const uint32_t code = kHuffSymTable[sym].code_msb;
            int node = 0;
            for (uint8_t i = 0; i < bits; ++i) {
                const int bit = (code >> (31 - i)) & 1;
                int16_t& child = t[node].next[bit];
                if (child < 0) {
                    child = static_cast<int16_t>(t.size());
                    t.emplace_back();
                }
                node = child;
            }
            t[node].sym = static_cast<int16_t>(sym);
        }
        return t;
    }();
    return trie;
}

bool Hpack::HuffmanDecode(const uint8_t* data, size_t len, std::string* out) {
    if (!out) return false;
    out->clear();
    if (!data || len == 0) return true;

    const auto& trie = GetHuffTrie();
    int node = 0;
    int bitsSinceLastSymbol = 0;
    bool tailAllOnes = true;

    for (size_t i = 0; i < len; ++i) {
        const uint8_t b = data[i];
        for (int bitpos = 7; bitpos >= 0; --bitpos) {
            const int bit = (b >> bitpos) & 1;
            ++bitsSinceLastSymbol;
            if (bit == 0) tailAllOnes = false;
            if (bitsSinceLastSymbol > 30) return false;

            const int16_t next = trie[node].next[bit];
            if (next < 0) return false;
            node = next;
            if (trie[node].sym >= 0) {
                const int sym = trie[node].sym;
                if (sym == 256) return false; // EOS must not appear
                out->push_back(static_cast<char>(sym));
                node = 0;
                bitsSinceLastSymbol = 0;
                tailAllOnes = true;
            }
        }
    }

    if (node != 0) {
        // Valid padding is a prefix of EOS (all ones) and at most 7 bits.
        if (bitsSinceLastSymbol > 7) return false;
        if (!tailAllOnes) return false;
    }
    return true;
}

bool Hpack::DecodeString(const uint8_t* data, size_t len, std::string* out, size_t* outConsumed) {
    if (!data || len == 0 || !out || !outConsumed) return false;
    const bool huff = (data[0] & 0x80u) != 0;
    uint32_t n = 0;
    size_t used = 0;
    if (!DecodeInteger(data, len, 7, &n, &used)) return false;
    if (used + n > len) return false;
    if (!huff) {
        out->assign(reinterpret_cast<const char*>(data + used), reinterpret_cast<const char*>(data + used + n));
        *outConsumed = used + n;
        return true;
    }
    std::string decoded;
    if (!HuffmanDecode(data + used, n, &decoded)) return false;
    *out = std::move(decoded);
    *outConsumed = used + n;
    return true;
}

void Hpack::EncodeInteger(std::vector<uint8_t>* out, uint32_t value, uint8_t prefixBits, uint8_t firstByteMask) {
    const uint8_t prefixMax = static_cast<uint8_t>((1u << prefixBits) - 1u);
    if (value < prefixMax) {
        out->push_back(static_cast<uint8_t>(firstByteMask | value));
        return;
    }
    out->push_back(static_cast<uint8_t>(firstByteMask | prefixMax));
    value -= prefixMax;
    while (value >= 128) {
        out->push_back(static_cast<uint8_t>((value & 0x7F) | 0x80));
        value >>= 7;
    }
    out->push_back(static_cast<uint8_t>(value));
}

void Hpack::EncodeStringRaw(std::vector<uint8_t>* out, const std::string& s) {
    // huffman flag = 0
    EncodeInteger(out, static_cast<uint32_t>(s.size()), 7, 0x00);
    out->insert(out->end(), s.begin(), s.end());
}

std::vector<uint8_t> Hpack::EncodeNoIndex(const std::vector<Header>& headers) {
    std::vector<uint8_t> out;
    for (const auto& h : headers) {
        // Literal Header Field without Indexing - New Name: 0000
        // First byte: 0000xxxx with 4-bit prefix for name index (0 = new name).
        EncodeInteger(&out, 0, 4, 0x00);
        EncodeStringRaw(&out, h.name);
        EncodeStringRaw(&out, h.value);
    }
    return out;
}

bool Hpack::Decode(const uint8_t* data, size_t len, std::vector<Header>* outHeaders) {
    if (!outHeaders) return false;
    outHeaders->clear();
    size_t off = 0;
    while (off < len) {
        const uint8_t b = data[off];
        if (b & 0x80u) {
            // Indexed Header Field Representation (1xxxxxxx)
            uint32_t idx = 0;
            size_t used = 0;
            if (!DecodeInteger(data + off, len - off, 7, &idx, &used)) return false;
            off += used;
            const Entry* e = GetByIndex(idx);
            if (!e) return false;
            outHeaders->push_back({e->name, e->value});
            continue;
        }
        if ((b & 0xE0u) == 0x20u) {
            // Dynamic Table Size Update (001xxxxx)
            uint32_t sz = 0;
            size_t used = 0;
            if (!DecodeInteger(data + off, len - off, 5, &sz, &used)) return false;
            off += used;
            UpdateMaxSize(sz);
            continue;
        }

        const bool incIndex = (b & 0x40u) != 0;      // 01xxxxxx
        const bool neverIndex = (b & 0x10u) != 0;    // 0001xxxx (we treat as without indexing)
        uint8_t prefix = 0;
        if (incIndex) prefix = 6;
        else if ((b & 0xF0u) == 0x00u || (b & 0xF0u) == 0x10u) prefix = 4;
        else return false;

        uint32_t nameIndex = 0;
        size_t used = 0;
        if (!DecodeInteger(data + off, len - off, prefix, &nameIndex, &used)) return false;
        off += used;

        std::string name;
        if (nameIndex != 0) {
            const Entry* e = GetByIndex(nameIndex);
            if (!e) return false;
            name = e->name;
        } else {
            size_t c = 0;
            if (!DecodeString(data + off, len - off, &name, &c)) return false;
            off += c;
        }

        std::string value;
        {
            size_t c = 0;
            if (!DecodeString(data + off, len - off, &value, &c)) return false;
            off += c;
        }

        outHeaders->push_back({name, value});
        if (incIndex && !neverIndex) {
            AddDynamic(name, value);
        }
    }
    return true;
}

} // namespace protocol
} // namespace proxy
