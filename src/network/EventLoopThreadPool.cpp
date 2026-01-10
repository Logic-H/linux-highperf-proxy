#include "proxy/network/EventLoopThreadPool.h"
#include "proxy/network/EventLoopThread.h"
#include "proxy/network/EventLoop.h"

namespace proxy {
namespace network {

EventLoopThreadPool::EventLoopThreadPool(EventLoop* baseLoop, const std::string& nameArg)
    : baseLoop_(baseLoop),
      name_(nameArg),
      started_(false),
      numThreads_(0),
      next_(0) {
}

EventLoopThreadPool::~EventLoopThreadPool() {
    // threads_ cleanup handled by unique_ptr
}

void EventLoopThreadPool::Start() {
    started_ = true;

    for (int i = 0; i < numThreads_; ++i) {
        char buf[name_.size() + 32];
        snprintf(buf, sizeof buf, "%s%d", name_.c_str(), i);
        EventLoopThread* t = new EventLoopThread(buf);
        threads_.push_back(std::unique_ptr<EventLoopThread>(t));
        loops_.push_back(t->StartLoop());
    }
}

EventLoop* EventLoopThreadPool::GetNextLoop() {
    EventLoop* loop = baseLoop_;

    if (!loops_.empty()) {
        loop = loops_[next_];
        ++next_;
        if (static_cast<size_t>(next_) >= loops_.size()) {
            next_ = 0;
        }
    }
    return loop;
}

} // namespace network
} // namespace proxy
