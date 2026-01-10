#pragma once

#include "proxy/common/noncopyable.h"
#include <string>
#include <vector>
#include <memory>

namespace proxy {
namespace network {

class EventLoop;
class EventLoopThread;

class EventLoopThreadPool : proxy::common::noncopyable {
public:
    EventLoopThreadPool(EventLoop* baseLoop, const std::string& nameArg);
    ~EventLoopThreadPool();

    void SetThreadNum(int numThreads) { numThreads_ = numThreads; }
    void Start();

    // Round Robin
    EventLoop* GetNextLoop();

    bool started() const { return started_; }
    const std::string& name() const { return name_; }

private:
    EventLoop* baseLoop_;
    std::string name_;
    bool started_;
    int numThreads_;
    int next_;
    std::vector<std::unique_ptr<EventLoopThread>> threads_;
    std::vector<EventLoop*> loops_;
};

} // namespace network
} // namespace proxy
