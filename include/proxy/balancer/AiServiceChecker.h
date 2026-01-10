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

// Performs a simple HTTP GET to fetch AI service status/metrics (GPU/VRAM/model state).
// Intended to satisfy project.txt "AI服务检查：GPU利用率、显存占用、模型加载状态".
class AiServiceChecker : proxy::common::noncopyable,
                         public std::enable_shared_from_this<AiServiceChecker> {
public:
    struct Result {
        bool hasQueueLen{false};
        int queueLen{0};

        bool hasGpu{false};
        double gpuUtil01{0.0}; // [0..1]
        int vramUsedMb{0};
        int vramTotalMb{0};

        bool hasModelLoaded{false};
        bool modelLoaded{false};

        // Optional: model name currently loaded/active on the backend.
        // Supported JSON fields: "model", "model_name", "loaded_model".
        bool hasModelName{false};
        std::string modelName;

        // Optional: model version currently loaded/active on the backend.
        // Supported JSON fields: "model_version", "version", "modelVersion".
        bool hasModelVersion{false};
        std::string modelVersion;
    };

    using CheckCallback = std::function<void(bool ok, const proxy::network::InetAddress& addr, const Result& r)>;

    AiServiceChecker(proxy::network::EventLoop* loop,
                     double timeoutSec = 2.0,
                     std::string hostHeader = "127.0.0.1",
                     std::string path = "/ai/status",
                     int okStatusMin = 200,
                     int okStatusMax = 399);

    void Check(const proxy::network::InetAddress& addr, CheckCallback cb);

private:
    enum class State { kConnecting, kSending, kReading };

    struct CheckContext {
        int sockfd{-1};
        int timerfd{-1};
        std::shared_ptr<proxy::network::Channel> connChannel;
        std::shared_ptr<proxy::network::Channel> timerChannel;
        CheckCallback cb;
        proxy::network::InetAddress addr;

        State state{State::kConnecting};
        std::string out;
        size_t outOffset{0};
        std::string in;
        std::atomic<bool> finished{false};
    };

    void OnWritable(std::shared_ptr<CheckContext> ctx);
    void OnReadable(std::shared_ptr<CheckContext> ctx, std::chrono::system_clock::time_point);
    void OnError(std::shared_ptr<CheckContext> ctx);
    void OnTimeout(std::shared_ptr<CheckContext> ctx);
    bool CleanUp(std::shared_ptr<CheckContext> ctx);

    static int ParseHttpStatusCode(const std::string& statusLine);
    static bool ParseJsonResult(const std::string& body, Result* out);

    proxy::network::EventLoop* loop_;
    double timeoutSec_;
    std::string hostHeader_;
    std::string path_;
    int okStatusMin_;
    int okStatusMax_;
};

} // namespace balancer
} // namespace proxy
