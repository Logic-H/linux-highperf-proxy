#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace proxy {
namespace monitor {

// Simple IPv4 access control + token allowlist.
class AccessControl {
public:
    enum class IpMode { kOff, kAllowList, kDenyList };

    struct Config {
        IpMode ipMode{IpMode::kOff};
        std::vector<std::string> cidrs; // e.g. "127.0.0.1/32", "10.0.0.0/8"
        bool requireToken{false};
        std::string tokenHeader{"X-Api-Token"};
        std::vector<std::string> validTokens;
        bool requireApiKey{false};
        std::string apiKeyHeader{"X-Api-Key"};
        std::vector<std::string> validApiKeys;
    };

    explicit AccessControl(Config cfg);

    // Returns true if ip+token+apiKey are allowed.
    bool Allow(const std::string& peerIp, const std::string& token) const;
    bool Allow(const std::string& peerIp, const std::string& token, const std::string& apiKey) const;

    const Config& config() const { return cfg_; }

    static bool ParseIpv4(const std::string& ip, std::uint32_t* out);
    static bool ParseCidr(const std::string& cidr, std::uint32_t* network, std::uint32_t* mask);

private:
    bool IpAllowed(std::uint32_t ip) const;
    bool TokenAllowed(const std::string& token) const;
    bool ApiKeyAllowed(const std::string& apiKey) const;

    struct CidrNet {
        std::uint32_t network{0};
        std::uint32_t mask{0};
    };

    Config cfg_;
    std::vector<CidrNet> nets_;
};

} // namespace monitor
} // namespace proxy
