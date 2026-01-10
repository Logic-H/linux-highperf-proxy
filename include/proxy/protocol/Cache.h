#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace proxy {
namespace protocol {

// Cache integration (best-effort):
// - Supports Redis (RESP) and Memcached (text) backends.
// - Intended for simple GET response caching; operations are bounded by timeouts.
class Cache {
public:
    struct Config {
        bool enabled{false};
        std::string backend{"off"}; // off|redis|memcached
        std::string host{"127.0.0.1"};
        uint16_t port{0};
        int ttlSec{60};
        int timeoutMs{5};           // per operation (connect+io)
        size_t maxValueBytes{256 * 1024};
    };

    void Configure(const Config& cfg);
    const Config& config() const { return cfg_; }

    bool Enabled() const { return cfg_.enabled && cfg_.port != 0 && cfg_.backend != "off"; }

    // Returns true on cache hit.
    bool Get(const std::string& key, std::string* valueOut) const;
    void Set(const std::string& key, const std::string& value) const;

private:
    Config cfg_{};
};

} // namespace protocol
} // namespace proxy

