#include "proxy/network/Poller.h"
#include "proxy/network/Channel.h"

namespace proxy {
namespace network {

Poller::Poller(EventLoop* loop) : loop_(loop) {}

Poller::~Poller() = default;

bool Poller::HasChannel(Channel* channel) const {
    auto it = channels_.find(channel->fd());
    return it != channels_.end() && it->second == channel;
}

} // namespace network
} // namespace proxy
