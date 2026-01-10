#include "proxy/network/EventLoop.h"
#include "proxy/common/Logger.h"
#include "proxy/network/Poller.h"
#include "proxy/network/Channel.h"

#include <sys/eventfd.h>
#include <unistd.h>
#include <fcntl.h>
#include <algorithm>

namespace proxy {
namespace network {

__thread EventLoop* t_loopInThisThread = nullptr;

const int kPollTimeMs = 10000;

int CreateEventfd() {
    int evtfd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (evtfd < 0) {
        LOG_FATAL << "Failed in eventfd";
    }
    return evtfd;
}

EventLoop* EventLoop::GetEventLoopOfCurrentThread() {
    return t_loopInThisThread;
}

EventLoop::EventLoop()
    : looping_(false),
      quit_(false),
      calling_pending_functors_(false),
      thread_id_(std::this_thread::get_id()),
      poller_(Poller::NewDefaultPoller(this)),
      wakeup_fd_(CreateEventfd()),
      wakeup_channel_(new Channel(this, wakeup_fd_)) {
    
    LOG_DEBUG << "EventLoop created " << this << " in thread " << thread_id_;
    
    if (t_loopInThisThread) {
        LOG_FATAL << "Another EventLoop " << t_loopInThisThread << " exists in this thread " << thread_id_;
    } else {
        t_loopInThisThread = this;
    }

    wakeup_channel_->SetReadCallback(std::bind(&EventLoop::HandleRead, this));
    wakeup_channel_->EnableReading();
}

EventLoop::~EventLoop() {
    wakeup_channel_->DisableAll();
    wakeup_channel_->Remove();
    ::close(wakeup_fd_);
    t_loopInThisThread = nullptr;
}

void EventLoop::Loop() {
    looping_ = true;
    quit_ = false;
    LOG_INFO << "EventLoop " << this << " start looping";

    while (!quit_) {
        active_channels_.clear();
        poller_->Poll(kPollTimeMs, &active_channels_);
        for (Channel* channel : active_channels_) {
            channel->HandleEvent(std::chrono::system_clock::now());
        }
        DoPendingFunctors();
    }

    LOG_INFO << "EventLoop " << this << " stop looping";
    looping_ = false;
}

void EventLoop::Quit() {
    quit_ = true;
    if (!IsInLoopThread()) {
        WakeUp();
    }
}

void EventLoop::RunInLoop(Functor cb) {
    if (IsInLoopThread()) {
        cb();
    } else {
        QueueInLoop(std::move(cb));
    }
}

void EventLoop::QueueInLoop(Functor cb) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_functors_.emplace_back(std::move(cb));
    }

    if (!IsInLoopThread() || calling_pending_functors_) {
        WakeUp();
    }
}

void EventLoop::WakeUp() {
    uint64_t one = 1;
    ssize_t n = ::write(wakeup_fd_, &one, sizeof one);
    if (n != sizeof one) {
        LOG_ERROR << "EventLoop::wakeup() writes " << n << " bytes instead of 8";
    } else {
        LOG_DEBUG << "EventLoop::WakeUp() wrote to fd " << wakeup_fd_;
    }
}

void EventLoop::HandleRead() {
    uint64_t one = 1;
    ssize_t n = ::read(wakeup_fd_, &one, sizeof one);
    if (n != sizeof one) {
        LOG_ERROR << "EventLoop::handleRead() reads " << n << " bytes instead of 8";
    }
}

void EventLoop::UpdateChannel(Channel* channel) {
    // assert(channel->ownerLoop() == this);
    poller_->UpdateChannel(channel);
}

void EventLoop::RemoveChannel(Channel* channel) {
    // assert(channel->ownerLoop() == this);
    poller_->RemoveChannel(channel);
}

bool EventLoop::HasChannel(Channel* channel) {
    // assert(channel->ownerLoop() == this);
    return poller_->HasChannel(channel);
}

void EventLoop::DoPendingFunctors() {
    std::vector<Functor> functors;
    calling_pending_functors_ = true;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        functors.swap(pending_functors_);
    }

    for (const auto& functor : functors) {
        functor();
    }
    calling_pending_functors_ = false;
}

} // namespace network
} // namespace proxy
