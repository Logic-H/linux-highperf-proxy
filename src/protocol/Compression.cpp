#include "proxy/protocol/Compression.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#if PROXY_WITH_ZLIB
#include <zlib.h>
#endif

namespace proxy {
namespace protocol {

static std::string ToLowerCopy(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) out.push_back(static_cast<char>(std::tolower(c)));
    return out;
}

Compression::Encoding Compression::ParseContentEncoding(const std::string& v) {
    const std::string lv = ToLowerCopy(v);
    if (lv.find("gzip") != std::string::npos) return Encoding::kGzip;
    if (lv.find("deflate") != std::string::npos) return Encoding::kDeflate;
    if (lv.empty() || lv.find("identity") != std::string::npos) return Encoding::kIdentity;
    return Encoding::kUnknown;
}

static bool InflateAll(const uint8_t* data, size_t len, int windowBits, std::string* out) {
#if PROXY_WITH_ZLIB
    if (!out) return false;
    out->clear();
    z_stream zs;
    std::memset(&zs, 0, sizeof(zs));
    zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(data));
    zs.avail_in = static_cast<uInt>(len);
    if (inflateInit2(&zs, windowBits) != Z_OK) return false;

    char buf[16384];
    int ret = Z_OK;
    while (ret != Z_STREAM_END) {
        zs.next_out = reinterpret_cast<Bytef*>(buf);
        zs.avail_out = sizeof(buf);
        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END) {
            inflateEnd(&zs);
            return false;
        }
        const size_t produced = sizeof(buf) - zs.avail_out;
        if (produced) out->append(buf, buf + produced);
    }
    inflateEnd(&zs);
    return true;
#else
    (void)data;
    (void)len;
    (void)windowBits;
    if (out) out->clear();
    return false;
#endif
}

static bool DeflateAll(const uint8_t* data, size_t len, int windowBits, std::string* out) {
#if PROXY_WITH_ZLIB
    if (!out) return false;
    out->clear();
    z_stream zs;
    std::memset(&zs, 0, sizeof(zs));
    zs.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(data));
    zs.avail_in = static_cast<uInt>(len);
    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, windowBits, 8, Z_DEFAULT_STRATEGY) != Z_OK) return false;

    char buf[16384];
    int ret = Z_OK;
    while (ret != Z_STREAM_END) {
        zs.next_out = reinterpret_cast<Bytef*>(buf);
        zs.avail_out = sizeof(buf);
        ret = deflate(&zs, Z_FINISH);
        if (ret != Z_OK && ret != Z_STREAM_END) {
            deflateEnd(&zs);
            return false;
        }
        const size_t produced = sizeof(buf) - zs.avail_out;
        if (produced) out->append(buf, buf + produced);
    }
    deflateEnd(&zs);
    return true;
#else
    (void)data;
    (void)len;
    (void)windowBits;
    if (out) out->clear();
    return false;
#endif
}

bool Compression::Decompress(Encoding enc, const uint8_t* data, size_t len, std::string* out) {
    if (!out) return false;
    if (enc == Encoding::kIdentity) {
        out->assign(reinterpret_cast<const char*>(data), reinterpret_cast<const char*>(data + len));
        return true;
    }
#if PROXY_WITH_ZLIB
    if (enc == Encoding::kGzip) {
        return InflateAll(data, len, 16 + MAX_WBITS, out);
    }
    if (enc == Encoding::kDeflate) {
        return InflateAll(data, len, MAX_WBITS, out);
    }
#else
    (void)data;
    (void)len;
#endif
    return false;
}

bool Compression::Decompress(Encoding enc, const std::string& in, std::string* out) {
    return Decompress(enc, reinterpret_cast<const uint8_t*>(in.data()), in.size(), out);
}

bool Compression::Compress(Encoding enc, const uint8_t* data, size_t len, std::string* out) {
    if (!out) return false;
    if (enc == Encoding::kIdentity) {
        out->assign(reinterpret_cast<const char*>(data), reinterpret_cast<const char*>(data + len));
        return true;
    }
#if PROXY_WITH_ZLIB
    if (enc == Encoding::kGzip) {
        return DeflateAll(data, len, 16 + MAX_WBITS, out);
    }
    if (enc == Encoding::kDeflate) {
        return DeflateAll(data, len, MAX_WBITS, out);
    }
#else
    (void)data;
    (void)len;
#endif
    return false;
}

bool Compression::Compress(Encoding enc, const std::string& in, std::string* out) {
    return Compress(enc, reinterpret_cast<const uint8_t*>(in.data()), in.size(), out);
}

} // namespace protocol
} // namespace proxy
