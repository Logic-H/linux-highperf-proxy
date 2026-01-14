#include "proxy/network/EpollPoller.h"
#include "proxy/network/PollPoller.h"
#include "proxy/network/SelectPoller.h"
#include "proxy/network/Channel.h"
#include "proxy/common/Logger.h"

#include <sys/epoll.h>
#include <unistd.h>
#include <cstring>
#include <cstdlib>

#if PROXY_WITH_URING
#include "proxy/network/UringPoller.h"
#endif

namespace proxy {
namespace network {

const int kNew = -1;
const int kAdded = 1;
const int kDeleted = 2;

EpollPoller::EpollPoller(EventLoop* loop)
    : Poller(loop),
      epollfd_(::epoll_create1(EPOLL_CLOEXEC)),
      events_(kInitEventListSize) {
    if (epollfd_ < 0) {
        LOG_FATAL << "EpollPoller::EpollPoller epoll_create1 failed";
    }
}

EpollPoller::~EpollPoller() {
    ::close(epollfd_);
}

std::chrono::system_clock::time_point EpollPoller::Poll(int timeout_ms, ChannelList* active_channels) {
    int num_events = ::epoll_wait(epollfd_, events_.data(), static_cast<int>(events_.size()), timeout_ms);
    int saved_errno = errno;
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now();

    if (num_events > 0) {
        LOG_DEBUG << num_events << " events happened";
        FillActiveChannels(num_events, active_channels);
        if (static_cast<size_t>(num_events) == events_.size()) {
            events_.resize(events_.size() * 2);
        }
    } else if (num_events == 0) {
        LOG_DEBUG << "nothing happened";
    } else {
        if (saved_errno != EINTR) {
            errno = saved_errno;
            LOG_ERROR << "EpollPoller::Poll()";
        }
    }
    return now;
}

void EpollPoller::FillActiveChannels(int num_events, ChannelList* active_channels) const {
    for (int i = 0; i < num_events; ++i) {
        Channel* channel = static_cast<Channel*>(events_[i].data.ptr);
        channel->set_revents(events_[i].events);
        active_channels->push_back(channel);
    }
}

void EpollPoller::UpdateChannel(Channel* channel) {
    const int index = channel->index();
    LOG_DEBUG << "fd = " << channel->fd() << " events = " << channel->events() << " index = " << index;

    if (index == kNew || index == kDeleted) {
        // a new one, add with EPOLL_CTL_ADD
        int fd = channel->fd();
        if (index == kNew) {
            // assert(channels_.find(fd) == channels_.end());
            channels_[fd] = channel;
        }

        channel->set_index(kAdded);
        Update(EPOLL_CTL_ADD, channel);
    } else {
        // update existing one with EPOLL_CTL_MOD/DEL
        int fd = channel->fd();
        (void)fd;
        // assert(channels_.find(fd) != channels_.end());
        // assert(channels_[fd] == channel);
        // assert(index == kAdded);
        if (channel->IsNoneEvent()) {
            Update(EPOLL_CTL_DEL, channel);
            channel->set_index(kDeleted);
        } else {
            Update(EPOLL_CTL_MOD, channel);
        }
    }
}

void EpollPoller::RemoveChannel(Channel* channel) {
    int fd = channel->fd();
    LOG_DEBUG << "RemoveChannel fd = " << fd;
    // assert(channels_.find(fd) != channels_.end());
    // assert(channels_[fd] == channel);
    // assert(channel->IsNoneEvent());
    int index = channel->index();
    // assert(index == kAdded || index == kDeleted);
    size_t n = channels_.erase(fd);
    (void)n;
    // assert(n == 1);

    if (index == kAdded) {
        Update(EPOLL_CTL_DEL, channel);
    }
    channel->set_index(kNew);
}

void EpollPoller::Update(int operation, Channel* channel) {
    struct epoll_event event;
    memset(&event, 0, sizeof(event));
    event.events = channel->events();
    event.data.ptr = channel;
    int fd = channel->fd();
    if (::epoll_ctl(epollfd_, operation, fd, &event) < 0) {
        if (operation == EPOLL_CTL_DEL) {
            LOG_ERROR << "epoll_ctl op=" << operation << " fd=" << fd;
        } else {
            LOG_FATAL << "epoll_ctl op=" << operation << " fd=" << fd;
        }
    }
}

Poller* Poller::NewDefaultPoller(EventLoop* loop) {
#if PROXY_WITH_URING
    if (::getenv("PROXY_USE_URING")) {
        LOG_INFO << "Using UringPoller";
        return new UringPoller(loop);
    }
#else
    if (::getenv("PROXY_USE_URING")) {
        LOG_WARN << "PROXY_USE_URING is set but built without io_uring support (PROXY_WITH_URING=0); falling back.";
    }
#endif
    if (::getenv("PROXY_USE_SELECT")) {
        LOG_INFO << "Using SelectPoller";
        return new SelectPoller(loop);
    } else if (::getenv("PROXY_USE_POLL")) {
        LOG_INFO << "Using PollPoller";
        return new PollPoller(loop);
    } else {
        LOG_INFO << "Using EpollPoller";
        return new EpollPoller(loop);
    }
}

} // namespace network
} // namespace proxy
