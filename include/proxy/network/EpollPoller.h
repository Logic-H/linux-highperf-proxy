#pragma once

#include "proxy/network/Poller.h"
#include <vector>
#include <sys/epoll.h>

namespace proxy {
namespace network {

class EpollPoller : public Poller {
public:
    EpollPoller(EventLoop* loop);
    ~EpollPoller() override;

    std::chrono::system_clock::time_point Poll(int timeout_ms, ChannelList* active_channels) override;
    void UpdateChannel(Channel* channel) override;
    void RemoveChannel(Channel* channel) override;

private:
    static const int kInitEventListSize = 16;

    void FillActiveChannels(int num_events, ChannelList* active_channels) const;
    void Update(int operation, Channel* channel);

    int epollfd_;
    using EventList = std::vector<struct epoll_event>;
    EventList events_;
};

} // namespace network
} // namespace proxy
