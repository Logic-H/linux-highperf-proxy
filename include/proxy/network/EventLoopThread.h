#pragma once

#include "proxy/common/noncopyable.h"
#include <mutex>
#include <condition_variable>
#include <thread>
#include <string>

namespace proxy {
namespace network {

class EventLoop;

class EventLoopThread : proxy::common::noncopyable {
public:
    EventLoopThread(const std::string& name = std::string());
    ~EventLoopThread();

    EventLoop* StartLoop();

private:
    void ThreadFunc();

    EventLoop* loop_;
    bool exiting_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cond_;
    std::string name_;
};

} // namespace network
} // namespace proxy
