#include "proxy/protocol/Cookie.h"

#include <cctype>

namespace proxy {
namespace protocol {

static inline void TrimInPlace(std::string& s) {
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    size_t j = s.size();
    while (j > i && std::isspace(static_cast<unsigned char>(s[j - 1]))) --j;
    s = s.substr(i, j - i);
}

std::string GetCookieValue(const std::string& cookieHeader, const std::string& name) {
    if (name.empty() || cookieHeader.empty()) return "";

    size_t pos = 0;
    while (pos < cookieHeader.size()) {
        size_t next = cookieHeader.find(';', pos);
        if (next == std::string::npos) next = cookieHeader.size();

        std::string part = cookieHeader.substr(pos, next - pos);
        TrimInPlace(part);
        if (!part.empty()) {
            size_t eq = part.find('=');
            if (eq != std::string::npos) {
                std::string k = part.substr(0, eq);
                std::string v = part.substr(eq + 1);
                TrimInPlace(k);
                TrimInPlace(v);
                if (k == name) return v;
            }
        }

        pos = next + 1;
    }
    return "";
}

} // namespace protocol
} // namespace proxy

