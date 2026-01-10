#pragma once

#include "proxy/network/Poller.h"
#include <vector>
#include <liburing.h>

namespace proxy {
namespace network {

class UringPoller : public Poller {
public:
    UringPoller(EventLoop* loop);
    ~UringPoller() override;

    std::chrono::system_clock::time_point Poll(int timeout_ms, ChannelList* active_channels) override;
    void UpdateChannel(Channel* channel) override;
    void RemoveChannel(Channel* channel) override;

private:
    void FillActiveChannels(int num_events, ChannelList* active_channels) const;
    void Update(int operation, Channel* channel);
    
    struct io_uring ring_;
    const int kQueueDepth = 256;
};

} // namespace network
} // namespace proxy
