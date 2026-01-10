#include "proxy/network/InetAddress.h"
#include <cstring>
#include <arpa/inet.h>

namespace proxy {
namespace network {

InetAddress::InetAddress(uint16_t port, bool loopbackOnly) {
    std::memset(&addr_, 0, sizeof addr_);
    addr_.sin_family = AF_INET;
    in_addr_t ip = loopbackOnly ? INADDR_LOOPBACK : INADDR_ANY;
    addr_.sin_addr.s_addr = htonl(ip);
    addr_.sin_port = htons(port);
}

InetAddress::InetAddress(std::string ip, uint16_t port) {
    std::memset(&addr_, 0, sizeof addr_);
    addr_.sin_family = AF_INET;
    addr_.sin_port = htons(port);
    if (::inet_pton(AF_INET, ip.c_str(), &addr_.sin_addr) <= 0) {
        // Handle error or set to INADDR_NONE
    }
}

std::string InetAddress::toIp() const {
    char buf[64] = "";
    ::inet_ntop(AF_INET, &addr_.sin_addr, buf, sizeof buf);
    return buf;
}

std::string InetAddress::toIpPort() const {
    char buf[64] = "";
    ::inet_ntop(AF_INET, &addr_.sin_addr, buf, sizeof buf);
    size_t end = std::strlen(buf);
    uint16_t port = ntohs(addr_.sin_port);
    snprintf(buf + end, sizeof buf - end, ":%u", port);
    return buf;
}

uint16_t InetAddress::toPort() const {
    return ntohs(addr_.sin_port);
}

} // namespace network
} // namespace proxy
