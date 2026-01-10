#pragma once

#include "proxy/common/noncopyable.h"
#include "proxy/network/Socket.h"
#include "proxy/network/Channel.h"

#include <functional>

namespace proxy {
namespace network {

class EventLoop;
class InetAddress;

class Acceptor : proxy::common::noncopyable {
public:
    using NewConnectionCallback = std::function<void(int sockfd, const InetAddress&)>;

    Acceptor(EventLoop* loop, const InetAddress& listenAddr, bool reuseport);
    ~Acceptor();

    void SetNewConnectionCallback(const NewConnectionCallback& cb) {
        new_connection_callback_ = cb;
    }

    bool Listenning() const { return listenning_; }
    void Listen();

private:
    void HandleRead();

    EventLoop* loop_;
    Socket accept_socket_;
    Channel accept_channel_;
    NewConnectionCallback new_connection_callback_;
    bool listenning_;
};

} // namespace network
} // namespace proxy
