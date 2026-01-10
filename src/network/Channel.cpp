#include "proxy/network/Channel.h"
#include "proxy/network/EventLoop.h"
#include "proxy/common/Logger.h"

#include <sys/epoll.h>

namespace proxy {
namespace network {

const int Channel::kNoneEvent = 0;
const int Channel::kReadEvent = EPOLLIN | EPOLLPRI;
const int Channel::kWriteEvent = EPOLLOUT;

Channel::Channel(EventLoop* loop, int fd)
    : loop_(loop),
      fd_(fd),
      events_(0),
      revents_(0),
      index_(-1),
      added_to_loop_(false) {
}

Channel::~Channel() {
    // assert(!added_to_loop_);
}

void Channel::Update() {
    added_to_loop_ = true;
    loop_->UpdateChannel(this);
}

void Channel::Remove() {
    // assert(IsNoneEvent());
    added_to_loop_ = false;
    loop_->RemoveChannel(this);
}

void Channel::HandleEvent(std::chrono::system_clock::time_point receive_time) {
    if (revents_ & EPOLLHUP && !(revents_ & EPOLLIN)) {
        if (close_callback_) close_callback_();
    }

    if (revents_ & (EPOLLERR)) {
        if (error_callback_) error_callback_();
    }

    if (revents_ & (EPOLLIN | EPOLLPRI | EPOLLRDHUP)) {
        if (read_callback_) read_callback_(receive_time);
    }

    if (revents_ & EPOLLOUT) {
        if (write_callback_) write_callback_();
    }
}

} // namespace network
} // namespace proxy
