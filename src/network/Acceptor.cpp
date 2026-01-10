#include "proxy/network/Acceptor.h"
#include "proxy/network/InetAddress.h"
#include "proxy/network/EventLoop.h"
#include "proxy/common/Logger.h"

#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>

namespace proxy {
namespace network {

static int CreateNonblockingOrDie() {
    int sockfd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
    if (sockfd < 0) {
        LOG_FATAL << "sockets::CreateNonblockingOrDie";
    }
    return sockfd;
}

Acceptor::Acceptor(EventLoop* loop, const InetAddress& listenAddr, bool reuseport)
    : loop_(loop),
      accept_socket_(CreateNonblockingOrDie()),
      accept_channel_(loop, accept_socket_.fd()),
      listenning_(false) {
    
    accept_socket_.SetReuseAddr(true);
    accept_socket_.SetReusePort(reuseport);
    accept_socket_.BindAddress(listenAddr);
    
    accept_channel_.SetReadCallback(std::bind(&Acceptor::HandleRead, this));
}

Acceptor::~Acceptor() {
    accept_channel_.DisableAll();
    accept_channel_.Remove();
}

void Acceptor::Listen() {
    listenning_ = true;
    accept_socket_.Listen();
    accept_channel_.EnableReading();
}

void Acceptor::HandleRead() {
    InetAddress peerAddr;
    int connfd = accept_socket_.Accept(&peerAddr);
    if (connfd >= 0) {
        if (new_connection_callback_) {
            new_connection_callback_(connfd, peerAddr);
        } else {
            ::close(connfd);
        }
    } else {
        LOG_ERROR << "in Acceptor::HandleRead";
        if (errno == EMFILE) {
            LOG_ERROR << "sockfd reached limit";
        }
    }
}

} // namespace network
} // namespace proxy
