#pragma once

#include "proxy/common/noncopyable.h"
#include <functional>
#include <memory>
#include <chrono>

namespace proxy {
namespace network {

class EventLoop;

class Channel : proxy::common::noncopyable {
public:
    using EventCallback = std::function<void()>;
    using ReadEventCallback = std::function<void(std::chrono::system_clock::time_point)>;

    Channel(EventLoop* loop, int fd);
    ~Channel();

    void HandleEvent(std::chrono::system_clock::time_point receive_time);
    
    void SetReadCallback(ReadEventCallback cb) { read_callback_ = std::move(cb); }
    void SetWriteCallback(EventCallback cb) { write_callback_ = std::move(cb); }
    void SetCloseCallback(EventCallback cb) { close_callback_ = std::move(cb); }
    void SetErrorCallback(EventCallback cb) { error_callback_ = std::move(cb); }

    int fd() const { return fd_; }
    int events() const { return events_; }
    void set_revents(int revt) { revents_ = revt; }
    bool IsNoneEvent() const { return events_ == kNoneEvent; }

    void EnableReading() { events_ |= kReadEvent; Update(); }
    void DisableReading() { events_ &= ~kReadEvent; Update(); }
    void EnableWriting() { events_ |= kWriteEvent; Update(); }
    void DisableWriting() { events_ &= ~kWriteEvent; Update(); }
    void DisableAll() { events_ = kNoneEvent; Update(); }
    
    bool IsWriting() const { return events_ & kWriteEvent; }
    bool IsReading() const { return events_ & kReadEvent; }

    int index() const { return index_; }
    void set_index(int idx) { index_ = idx; }

    EventLoop* owner_loop() { return loop_; }
    void Remove();

private:
    void Update();

    static const int kNoneEvent;
    static const int kReadEvent;
    static const int kWriteEvent;

    EventLoop* loop_;
    const int fd_;
    int events_;
    int revents_;
    int index_; // Used by Poller
    bool added_to_loop_;

    ReadEventCallback read_callback_;
    EventCallback write_callback_;
    EventCallback close_callback_;
    EventCallback error_callback_;
};

} // namespace network
} // namespace proxy
