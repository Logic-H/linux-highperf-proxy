#pragma once

#include "proxy/common/noncopyable.h"
#include <vector>
#include <unordered_map>
#include <chrono>

namespace proxy {
namespace network {

class Channel;
class EventLoop;

class Poller : proxy::common::noncopyable {
public:
    using ChannelList = std::vector<Channel*>;

    Poller(EventLoop* loop);
    virtual ~Poller();

    virtual std::chrono::system_clock::time_point Poll(int timeout_ms, ChannelList* active_channels) = 0;
    virtual void UpdateChannel(Channel* channel) = 0;
    virtual void RemoveChannel(Channel* channel) = 0;
    virtual bool HasChannel(Channel* channel) const;

    static Poller* NewDefaultPoller(EventLoop* loop);

protected:
    using ChannelMap = std::unordered_map<int, Channel*>;
    ChannelMap channels_;

private:
    EventLoop* loop_;
};

} // namespace network
} // namespace proxy
