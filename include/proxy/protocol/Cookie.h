#pragma once

#include <string>

namespace proxy {
namespace protocol {

// Parse cookie header value and return the value of a cookie by name.
// Example: "a=1; b=2" + "b" => "2"
// Returns empty string if not found.
std::string GetCookieValue(const std::string& cookieHeader, const std::string& name);

} // namespace protocol
} // namespace proxy

