#include "proxy/monitor/AccessControl.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cstring>

namespace proxy {
namespace monitor {

static std::string TrimCopy(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    size_t j = s.size();
    while (j > i && std::isspace(static_cast<unsigned char>(s[j - 1]))) --j;
    return s.substr(i, j - i);
}

AccessControl::AccessControl(Config cfg) : cfg_(std::move(cfg)) {
    nets_.clear();
    nets_.reserve(cfg_.cidrs.size());
    for (const auto& c : cfg_.cidrs) {
        std::uint32_t net = 0, mask = 0;
        if (ParseCidr(c, &net, &mask)) {
            nets_.push_back(CidrNet{net, mask});
        }
    }
}

bool AccessControl::ParseIpv4(const std::string& ip, std::uint32_t* out) {
    if (!out) return false;
    in_addr addr;
    std::memset(&addr, 0, sizeof(addr));
    if (::inet_pton(AF_INET, ip.c_str(), &addr) != 1) return false;
    *out = ntohl(addr.s_addr);
    return true;
}

bool AccessControl::ParseCidr(const std::string& cidr, std::uint32_t* network, std::uint32_t* mask) {
    if (!network || !mask) return false;
    std::string s = TrimCopy(cidr);
    if (s.empty()) return false;

    auto slash = s.find('/');
    std::string ipPart = (slash == std::string::npos) ? s : s.substr(0, slash);
    std::string prefixPart = (slash == std::string::npos) ? "32" : s.substr(slash + 1);
    ipPart = TrimCopy(ipPart);
    prefixPart = TrimCopy(prefixPart);

    int prefix = -1;
    try {
        prefix = std::stoi(prefixPart);
    } catch (...) {
        return false;
    }
    if (prefix < 0 || prefix > 32) return false;

    std::uint32_t ip = 0;
    if (!ParseIpv4(ipPart, &ip)) return false;

    std::uint32_t m = (prefix == 0) ? 0u : (0xFFFFFFFFu << (32 - prefix));
    *mask = m;
    *network = ip & m;
    return true;
}

bool AccessControl::TokenAllowed(const std::string& token) const {
    if (!cfg_.requireToken) return true;
    if (token.empty()) return false;
    return std::find(cfg_.validTokens.begin(), cfg_.validTokens.end(), token) != cfg_.validTokens.end();
}

bool AccessControl::ApiKeyAllowed(const std::string& apiKey) const {
    if (!cfg_.requireApiKey) return true;
    if (apiKey.empty()) return false;
    return std::find(cfg_.validApiKeys.begin(), cfg_.validApiKeys.end(), apiKey) != cfg_.validApiKeys.end();
}

bool AccessControl::IpAllowed(std::uint32_t ip) const {
    if (cfg_.ipMode == IpMode::kOff) return true;
    if (nets_.empty()) {
        // No rules: allow in off; for allowlist/denylist treat as deny/allow respectively
        return cfg_.ipMode == IpMode::kDenyList;
    }
    bool matched = false;
    for (const auto& n : nets_) {
        if ((ip & n.mask) == n.network) {
            matched = true;
            break;
        }
    }
    if (cfg_.ipMode == IpMode::kAllowList) return matched;
    if (cfg_.ipMode == IpMode::kDenyList) return !matched;
    return true;
}

bool AccessControl::Allow(const std::string& peerIp, const std::string& token) const {
    return Allow(peerIp, token, "");
}

bool AccessControl::Allow(const std::string& peerIp, const std::string& token, const std::string& apiKey) const {
    std::uint32_t ip = 0;
    if (!ParseIpv4(peerIp, &ip)) {
        // Unknown ip format => reject when ip rules enabled, otherwise allow.
        if (cfg_.ipMode != IpMode::kOff) return false;
    } else {
        if (!IpAllowed(ip)) return false;
    }
    if (!TokenAllowed(token)) return false;
    if (!ApiKeyAllowed(apiKey)) return false;
    return true;
}

} // namespace monitor
} // namespace proxy
