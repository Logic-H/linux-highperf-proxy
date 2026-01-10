#include "proxy/network/Socket.h"
#include "proxy/network/InetAddress.h"
#include "proxy/common/Logger.h"

#include <unistd.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <cstring>

namespace proxy {
namespace network {

Socket::~Socket() {
    ::close(sockfd_);
}

void Socket::BindAddress(const InetAddress& localaddr) {
    if (::bind(sockfd_, localaddr.getSockAddr(), sizeof(struct sockaddr_in)) != 0) {
        LOG_FATAL << "Socket::BindAddress";
    }
}

void Socket::Listen() {
    if (::listen(sockfd_, SOMAXCONN) != 0) {
        LOG_FATAL << "Socket::Listen";
    }
}

int Socket::Accept(InetAddress* peeraddr) {
    struct sockaddr_in addr;
    socklen_t len = sizeof addr;
    std::memset(&addr, 0, sizeof addr);
    int connfd = ::accept4(sockfd_, (struct sockaddr*)&addr, &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (connfd >= 0) {
        peeraddr->setSockAddr(addr);
    }
    return connfd;
}

void Socket::ShutdownWrite() {
    if (::shutdown(sockfd_, SHUT_WR) < 0) {
        LOG_ERROR << "Socket::ShutdownWrite";
    }
}

void Socket::SetTcpNoDelay(bool on) {
    int optval = on ? 1 : 0;
    ::setsockopt(sockfd_, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof optval);
}

void Socket::SetReuseAddr(bool on) {
    int optval = on ? 1 : 0;
    ::setsockopt(sockfd_, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof optval);
}

void Socket::SetReusePort(bool on) {
    int optval = on ? 1 : 0;
    ::setsockopt(sockfd_, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof optval);
}

void Socket::SetKeepAlive(bool on) {
    int optval = on ? 1 : 0;
    ::setsockopt(sockfd_, SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof optval);
}

} // namespace network
} // namespace proxy
