#include "proxy/network/PollPoller.h"
#include "proxy/network/Channel.h"
#include "proxy/common/Logger.h"

#include <poll.h>
#include <algorithm>

namespace proxy {
namespace network {

PollPoller::PollPoller(EventLoop* loop)
    : Poller(loop) {
}

PollPoller::~PollPoller() = default;

std::chrono::system_clock::time_point PollPoller::Poll(int timeout_ms, ChannelList* active_channels) {
    int num_events = ::poll(pollfds_.data(), pollfds_.size(), timeout_ms);
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now();

    if (num_events > 0) {
        LOG_DEBUG << num_events << " events happened";
        FillActiveChannels(num_events, active_channels);
    } else if (num_events == 0) {
        LOG_DEBUG << "nothing happened";
    } else {
        if (errno != EINTR) {
            LOG_ERROR << "PollPoller::Poll()";
        }
    }
    return now;
}

void PollPoller::FillActiveChannels(int num_events, ChannelList* active_channels) const {
    for (auto it = pollfds_.begin(); it != pollfds_.end() && num_events > 0; ++it) {
        if (it->revents > 0) {
            --num_events;
            auto ch_it = channels_.find(it->fd);
            // assert(ch_it != channels_.end());
            Channel* channel = ch_it->second;
            // assert(channel->fd() == it->fd);
            channel->set_revents(it->revents);
            active_channels->push_back(channel);
        }
    }
}

void PollPoller::UpdateChannel(Channel* channel) {
    LOG_DEBUG << "fd = " << channel->fd() << " events = " << channel->events();
    if (channel->index() < 0) {
        // a new one, add to pollfds_
        struct pollfd pfd;
        pfd.fd = channel->fd();
        pfd.events = static_cast<short>(channel->events());
        pfd.revents = 0;
        pollfds_.push_back(pfd);
        int idx = static_cast<int>(pollfds_.size()) - 1;
        channel->set_index(idx);
        channels_[pfd.fd] = channel;
    } else {
        // update existing one
        int idx = channel->index();
        // assert(0 <= idx && idx < static_cast<int>(pollfds_.size()));
        struct pollfd& pfd = pollfds_[idx];
        // assert(pfd.fd == channel->fd() || pfd.fd == -channel->fd()-1);
        pfd.fd = channel->fd();
        pfd.events = static_cast<short>(channel->events());
        pfd.revents = 0;
        if (channel->IsNoneEvent()) {
            // ignore this pollfd
            pfd.fd = -channel->fd() - 1;
        }
    }
}

void PollPoller::RemoveChannel(Channel* channel) {
    LOG_DEBUG << "fd = " << channel->fd();
    int idx = channel->index();
    // assert(0 <= idx && idx < static_cast<int>(pollfds_.size()));
    const struct pollfd& pfd = pollfds_[idx];
    (void)pfd;
    // assert(pfd.fd == -channel->fd()-1 && channel->isNoneEvent());
    size_t n = channels_.erase(channel->fd());
    (void)n;
    // assert(n == 1);
    if (static_cast<size_t>(idx) == pollfds_.size() - 1) {
        pollfds_.pop_back();
    } else {
        int channelAtEnd = pollfds_.back().fd;
        std::iter_swap(pollfds_.begin() + idx, pollfds_.end() - 1);
        if (channelAtEnd < 0) {
            channelAtEnd = -channelAtEnd - 1;
        }
        channels_[channelAtEnd]->set_index(idx);
        pollfds_.pop_back();
    }
}

} // namespace network
} // namespace proxy
