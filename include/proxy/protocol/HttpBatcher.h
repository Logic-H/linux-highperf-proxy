#pragma once

#include "proxy/balancer/BackendConnectionPool.h"
#include "proxy/balancer/BackendManager.h"
#include "proxy/network/Channel.h"
#include "proxy/network/EventLoop.h"
#include "proxy/network/InetAddress.h"
#include "proxy/network/TcpConnection.h"
#include "proxy/protocol/HttpRequest.h"
#include "proxy/protocol/HttpResponseContext.h"

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace proxy {

struct ProxySessionContext;

namespace protocol {

// Opt-in HTTP JSON batching for AI-like small POST requests.
// - Groups requests by (backend, route, model).
// - Builds a single backend request with JSON array body: [req1, req2, ...].
// - Expects backend to return JSON array response with same number/order of items.
// - Splits response items back to each waiting client connection.
class HttpBatcher : public std::enable_shared_from_this<HttpBatcher> {
public:
    struct Config {
        bool enabled{false};
        int windowMs{2};
        size_t maxBatchSize{8};
        size_t maxBatchBytes{256 * 1024};
        size_t maxResponseBytes{1024 * 1024};

        // If non-empty, only these paths are eligible unless requireHeader is used.
        std::vector<std::string> paths;

        bool requireHeader{false};
        std::string headerName{"X-Batch"};
    };

    using ResumeCallback = std::function<void(const proxy::network::TcpConnectionPtr&)>;

    HttpBatcher(proxy::network::EventLoop* loop,
                balancer::BackendConnectionPool* backendPool,
                balancer::BackendManager* backendManager,
                Config cfg,
                ResumeCallback resumeCb);

    const Config& config() const { return cfg_; }

    bool IsEnabled() const { return cfg_.enabled; }

    // Returns true if request accepted for batching (caller should stop normal proxy flow).
    bool TryEnqueue(const proxy::network::TcpConnectionPtr& client,
                    const std::shared_ptr<ProxySessionContext>& ctx,
                    const proxy::protocol::HttpRequest& req,
                    const proxy::network::InetAddress& backendAddr,
                    const std::string& model,
                    bool closeAfterResponse,
                    std::chrono::steady_clock::time_point start);

    // Utility: split a JSON array body into item strings (each item is a valid JSON value substring).
    // Used by both batching (merge small requests) and batch-splitting (split large request).
    static bool splitJsonArrayItems(const std::string& body, std::vector<std::string>* outItems);

private:
    struct Item {
        proxy::network::TcpConnectionPtr client;
        std::weak_ptr<ProxySessionContext> ctx;
        bool closeAfterResponse{false};
        std::string body;
        std::chrono::steady_clock::time_point start{};
    };

    struct Group {
        std::string key;
        proxy::network::InetAddress backendAddr{0};
        std::string route;
        std::string model;

        std::vector<Item> items;
        size_t bytes{0};

        int timerfd{-1};
        std::shared_ptr<proxy::network::Channel> timerCh;
        bool flushing{false};
    };

    bool isEligible(const proxy::protocol::HttpRequest& req) const;
    bool pathAllowed(const std::string& path) const;
    static bool looksLikeJson(const std::string& body);

    void armTimer(Group* g);
    void disarmTimer(Group* g);
    void onTimerFired(const std::string& groupKey);
    void flushGroup(const std::string& groupKey);

    void failGroup(Group& g, const std::string& resp, bool backendKeepAlive);
    void deliverGroupResponse(Group& g, int statusCode, const std::string& body, bool backendKeepAlive);

    static bool decodeChunkedBody(const std::string& raw, std::string* outBody);
    static bool extractBodyFromRawHttp(const std::string& raw, std::string* outBody, bool* outChunked);

    proxy::network::EventLoop* loop_{nullptr};
    balancer::BackendConnectionPool* backendPool_{nullptr};
    balancer::BackendManager* backendManager_{nullptr};
    Config cfg_;
    ResumeCallback resumeCb_;

    std::map<std::string, Group> groups_;
};

} // namespace protocol
} // namespace proxy
