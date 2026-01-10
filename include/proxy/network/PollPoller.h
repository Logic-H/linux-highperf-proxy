#pragma once

#include "proxy/network/Poller.h"
#include <vector>
#include <poll.h>

namespace proxy {
namespace network {

class PollPoller : public Poller {
public:
    PollPoller(EventLoop* loop);
    ~PollPoller() override;

    std::chrono::system_clock::time_point Poll(int timeout_ms, ChannelList* active_channels) override;
    void UpdateChannel(Channel* channel) override;
    void RemoveChannel(Channel* channel) override;

private:
    void FillActiveChannels(int num_events, ChannelList* active_channels) const;

    using PollFdList = std::vector<struct pollfd>;
    PollFdList pollfds_;
};

} // namespace network
} // namespace proxy
