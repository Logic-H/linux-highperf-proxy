#pragma once

#include <vector>
#include <atomic>
#include <memory>
#include <thread>
#include <mutex>

#include "proxy/common/noncopyable.h"
#include "proxy/network/Channel.h"
#include "proxy/network/Poller.h"

namespace proxy {
namespace network {

class EventLoop : proxy::common::noncopyable {
public:
    using Functor = std::function<void()>;

    EventLoop();
    ~EventLoop();

    void Loop();
    void Quit();

    void RunInLoop(Functor cb);
    void QueueInLoop(Functor cb);

    void WakeUp();
    void UpdateChannel(Channel* channel);
    void RemoveChannel(Channel* channel);
    bool HasChannel(Channel* channel);

    bool IsInLoopThread() const { return thread_id_ == std::this_thread::get_id(); }

    static EventLoop* GetEventLoopOfCurrentThread();

private:
    void HandleRead(); // For wakeup
    void DoPendingFunctors();

    using ChannelList = std::vector<Channel*>;

    std::atomic_bool looping_;
    std::atomic_bool quit_;
    std::atomic_bool calling_pending_functors_;
    
    const std::thread::id thread_id_;
    std::unique_ptr<Poller> poller_;
    
    // wakeup fd
    int wakeup_fd_;
    std::unique_ptr<Channel> wakeup_channel_;

    ChannelList active_channels_;
    
    std::mutex mutex_;
    std::vector<Functor> pending_functors_;
};

} // namespace network
} // namespace proxy
