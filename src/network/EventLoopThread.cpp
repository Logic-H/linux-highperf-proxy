#include "proxy/network/EventLoopThread.h"
#include "proxy/network/EventLoop.h"

namespace proxy {
namespace network {

EventLoopThread::EventLoopThread(const std::string& name)
    : loop_(nullptr),
      exiting_(false),
      name_(name) {
}

EventLoopThread::~EventLoopThread() {
    exiting_ = true;
    if (loop_ != nullptr) {
        loop_->Quit();
        thread_.join();
    }
}

EventLoop* EventLoopThread::StartLoop() {
    thread_ = std::thread(std::bind(&EventLoopThread::ThreadFunc, this));

    EventLoop* loop = nullptr;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        while (loop_ == nullptr) {
            cond_.wait(lock);
        }
        loop = loop_;
    }

    return loop;
}

void EventLoopThread::ThreadFunc() {
    EventLoop loop;

    {
        std::unique_lock<std::mutex> lock(mutex_);
        loop_ = &loop;
        cond_.notify_one();
    }

    loop.Loop();
    
    std::lock_guard<std::mutex> lock(mutex_);
    loop_ = nullptr;
}

} // namespace network
} // namespace proxy
