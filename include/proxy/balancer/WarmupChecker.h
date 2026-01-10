#pragma once

#include "proxy/common/noncopyable.h"
#include "proxy/network/Channel.h"
#include "proxy/network/EventLoop.h"
#include "proxy/network/InetAddress.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace proxy {
namespace balancer {

// Sends a simple HTTP request to trigger model warmup/preload on a backend.
// Example:
//   POST /ai/warmup?model=xxx HTTP/1.1
//   Host: <hostHeader>
//   Connection: close
//   Content-Length: 0
//
// Callback ok=true when HTTP status code is in [okStatusMin, okStatusMax].
class WarmupChecker : proxy::common::noncopyable, public std::enable_shared_from_this<WarmupChecker> {
public:
    using Callback = std::function<void(bool ok, const proxy::network::InetAddress& addr)>;

    WarmupChecker(proxy::network::EventLoop* loop,
                  double timeoutSec = 2.0,
                  std::string hostHeader = "127.0.0.1",
                  std::string path = "/ai/warmup",
                  int okStatusMin = 200,
                  int okStatusMax = 399);

    void Warmup(const proxy::network::InetAddress& addr, const std::string& model, Callback cb);

private:
    enum class State { kConnecting, kSending, kReading };

    struct Ctx {
        int sockfd{-1};
        int timerfd{-1};
        std::shared_ptr<proxy::network::Channel> connCh;
        std::shared_ptr<proxy::network::Channel> timerCh;
        Callback cb;
        proxy::network::InetAddress addr;
        State state{State::kConnecting};
        std::string out;
        size_t outOff{0};
        std::string in;
        std::atomic<bool> finished{false};
    };

    void onWritable(std::shared_ptr<Ctx> ctx);
    void onReadable(std::shared_ptr<Ctx> ctx);
    void onError(std::shared_ptr<Ctx> ctx);
    void onTimeout(std::shared_ptr<Ctx> ctx);
    bool cleanup(std::shared_ptr<Ctx> ctx);

    static int parseStatusCode(const std::string& statusLine);
    static std::string urlEncode(const std::string& s);

    proxy::network::EventLoop* loop_{nullptr};
    double timeoutSec_{2.0};
    std::string hostHeader_;
    std::string path_;
    int okStatusMin_{200};
    int okStatusMax_{399};
};

} // namespace balancer
} // namespace proxy

