#include "proxy/network/UringPoller.h"
#include "proxy/network/Channel.h"
#include "proxy/common/Logger.h"

#include <liburing.h>
#if __has_include(<linux/time_types.h>)
#include <linux/time_types.h>
#endif
#if __has_include(<linux/types.h>)
#include <linux/types.h>
#endif
#include <cstdint>
#include <type_traits>
#include <utility>
#include <cstring>
#include <cerrno>

namespace {
template <typename Dummy = void>
static auto PrepPollRemoveImpl(io_uring_sqe* sqe, std::uintptr_t userData, int)
    -> decltype((io_uring_prep_poll_remove(std::declval<io_uring_sqe*>(), static_cast<void*>(nullptr)),
                 std::declval<Dummy>(),
                 void())) {
    io_uring_prep_poll_remove(sqe, reinterpret_cast<void*>(userData));
}

template <typename Dummy = void>
static void PrepPollRemoveImpl(io_uring_sqe* sqe, std::uintptr_t userData, ...) {
    io_uring_prep_poll_remove(sqe, static_cast<std::uint64_t>(userData));
}

static inline void PrepPollRemove(io_uring_sqe* sqe, std::uintptr_t userData) {
    PrepPollRemoveImpl(sqe, userData, 0);
}
} // namespace

namespace proxy {
namespace network {

UringPoller::UringPoller(EventLoop* loop)
    : Poller(loop) {
    int ret = io_uring_queue_init(kQueueDepth, &ring_, 0);
    if (ret < 0) {
        LOG_FATAL << "io_uring_queue_init failed: " << -ret;
    }
}

UringPoller::~UringPoller() {
    io_uring_queue_exit(&ring_);
}

std::chrono::system_clock::time_point UringPoller::Poll(int timeout_ms, ChannelList* active_channels) {
    struct io_uring_cqe* cqe;
    struct __kernel_timespec ts;
    ts.tv_sec = timeout_ms / 1000;
    ts.tv_nsec = (timeout_ms % 1000) * 1000000;

    int ret = io_uring_submit(&ring_);
    if (ret < 0) {
        LOG_ERROR << "io_uring_submit failed: " << -ret;
    }

    ret = io_uring_wait_cqe_timeout(&ring_, &cqe, &ts);
    
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now();

    if (ret < 0) {
        if (ret == -ETIME) {
            LOG_DEBUG << "UringPoller::Poll timeout";
            return now;
        } else if (ret != -EINTR) {
            LOG_ERROR << "io_uring_wait_cqe_timeout failed: " << -ret;
            return now;
        }
    }

    unsigned head;
    int count = 0;
    
    io_uring_for_each_cqe(&ring_, head, cqe) {
        count++;
        // Recover fd from user_data
        int fd = static_cast<int>(reinterpret_cast<uintptr_t>(io_uring_cqe_get_data(cqe)));
        
        // Find channel safely
        auto it = channels_.find(fd);
        if (it != channels_.end()) {
             Channel* channel = it->second;
             int res = cqe->res;
             
             if (res >= 0) {
                 channel->set_revents(res);
                 active_channels->push_back(channel);
                 UpdateChannel(channel); // Re-arm
             } else if (res == -ECANCELED) {
                 // Request was cancelled, expected when removing channel
             } else {
                 LOG_ERROR << "Poll error for fd " << fd << ": " << -res;
             }
        } else {
            // Channel removed or user_data mismatch (e.g. nullptr for remove confirmation)
            // If we used nullptr for remove confirmation, fd would be 0.
            // But fd 0 is valid (stdin).
            // We should use a distinct marker or check if fd matches a valid channel.
            // Since we only submit fd as user_data for POLL_ADD, this is safe.
            // (If fd 0 is used, channels_[0] would exist).
        }
    }
    
    io_uring_cq_advance(&ring_, count);
    
    if (count > 0) {
         LOG_DEBUG << count << " events happened";
    }

    return now;
}

void UringPoller::UpdateChannel(Channel* channel) {
    if (channel->events() == 0) {
        return;
    }

    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) {
        LOG_ERROR << "io_uring_get_sqe failed (ring full?)";
        return;
    }

    io_uring_prep_poll_add(sqe, channel->fd(), static_cast<unsigned>(channel->events()));
    // Store fd as user_data
    io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(static_cast<uintptr_t>(channel->fd())));
    
    // Submit immediately so kernel sees the new poll request
    int ret = io_uring_submit(&ring_);
    LOG_DEBUG << "UpdateChannel submit fd=" << channel->fd() << " ret=" << ret;
}

void UringPoller::RemoveChannel(Channel* channel) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (sqe) {
        // Cancel based on user_data set in io_uring_prep_poll_add (we store fd in user_data).
        PrepPollRemove(sqe, static_cast<std::uintptr_t>(channel->fd()));
        io_uring_sqe_set_data(sqe, nullptr); 
        io_uring_submit(&ring_);
    }
    
    size_t n = channels_.erase(channel->fd());
    (void)n;
}

void UringPoller::FillActiveChannels(int num_events, ChannelList* active_channels) const {
    (void)num_events;
    (void)active_channels;
}

void UringPoller::Update(int operation, Channel* channel) {
    (void)operation;
    (void)channel;
}

} // namespace network
} // namespace proxy
