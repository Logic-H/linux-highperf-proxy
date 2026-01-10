#pragma once

#include "proxy/network/Poller.h"

namespace proxy {
namespace network {

// Basic select()-based Poller.
// Note: select() is limited by FD_SETSIZE; intended as a baseline I/O model.
class SelectPoller : public Poller {
public:
    explicit SelectPoller(EventLoop* loop);
    ~SelectPoller() override;

    std::chrono::system_clock::time_point Poll(int timeout_ms, ChannelList* active_channels) override;
    void UpdateChannel(Channel* channel) override;
    void RemoveChannel(Channel* channel) override;
};

} // namespace network
} // namespace proxy

