#include "proxy/network/SelectPoller.h"
#include "proxy/network/Channel.h"
#include "proxy/common/Logger.h"

#include <sys/select.h>
#include <sys/epoll.h>
#include <cerrno>
#include <cstring>

namespace proxy {
namespace network {

SelectPoller::SelectPoller(EventLoop* loop) : Poller(loop) {}

SelectPoller::~SelectPoller() = default;

std::chrono::system_clock::time_point SelectPoller::Poll(int timeout_ms, ChannelList* active_channels) {
    fd_set readset;
    fd_set writeset;
    fd_set exceptset;
    FD_ZERO(&readset);
    FD_ZERO(&writeset);
    FD_ZERO(&exceptset);

    int maxfd = -1;
    for (const auto& [fd, channel] : channels_) {
        if (fd < 0) continue;
        maxfd = (fd > maxfd) ? fd : maxfd;
        FD_SET(fd, &exceptset);

        const int events = channel->events();
        if (events & (EPOLLIN | EPOLLPRI)) {
            FD_SET(fd, &readset);
        }
        if (events & EPOLLOUT) {
            FD_SET(fd, &writeset);
        }
    }

    struct timeval tv;
    struct timeval* tvp = nullptr;
    if (timeout_ms >= 0) {
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        tvp = &tv;
    }

    int num_events = 0;
    if (maxfd >= 0) {
        num_events = ::select(maxfd + 1, &readset, &writeset, &exceptset, tvp);
    } else {
        // No watched fds; still honor timeout to avoid busy-loop.
        num_events = ::select(0, &readset, &writeset, &exceptset, tvp);
    }

    int saved_errno = errno;
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now();

    if (num_events > 0) {
        for (auto& [fd, channel] : channels_) {
            int revents = 0;
            if (fd >= 0 && FD_ISSET(fd, &exceptset)) revents |= EPOLLERR;
            if (fd >= 0 && FD_ISSET(fd, &readset)) revents |= (EPOLLIN | EPOLLPRI);
            if (fd >= 0 && FD_ISSET(fd, &writeset)) revents |= EPOLLOUT;
            if (revents != 0) {
                channel->set_revents(revents);
                active_channels->push_back(channel);
            }
        }
    } else if (num_events == 0) {
        LOG_DEBUG << "SelectPoller: nothing happened";
    } else {
        if (saved_errno != EINTR) {
            errno = saved_errno;
            LOG_ERROR << "SelectPoller::Poll() " << std::strerror(saved_errno);
        }
    }
    return now;
}

void SelectPoller::UpdateChannel(Channel* channel) {
    const int fd = channel->fd();
    channels_[fd] = channel;
    channel->set_index(0);
}

void SelectPoller::RemoveChannel(Channel* channel) {
    const int fd = channel->fd();
    size_t n = channels_.erase(fd);
    (void)n;
    channel->set_index(-1);
}

} // namespace network
} // namespace proxy
