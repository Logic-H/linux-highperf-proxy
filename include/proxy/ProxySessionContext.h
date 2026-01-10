#pragma once

#include <memory>
#include <variant>
#include <any>
#include <chrono>
#include <unordered_map>
#include "proxy/protocol/HttpContext.h"
#include "proxy/balancer/BackendSession.h"
#include "proxy/balancer/BackendConnectionPool.h"
#include "proxy/protocol/HttpResponseContext.h"
#include "proxy/protocol/Http2Connection.h"

namespace proxy {

// The state of a proxy connection:
// 1. Handshake/Protocol Detection (Optional)
// 2. HTTP Parsing (L7)
// 3. Tunneling (L4 or after HTTP header decision)

struct ProxySessionContext {
    enum Type {
        kNone,
        kHttp,
        kHttp2,
        kTunnel
    };

    Type type = kNone;
    
    // For HTTP L7
    protocol::HttpContext httpContext;
    protocol::Http2Connection http2;
    
    // For Tunneling (L4 or established L7)
    std::shared_ptr<balancer::BackendSession> backendSession;
    
    // Buffering early data while deciding backend
    std::string buffer; 

    // For HTTP proxying with connection pooling (one outstanding request at a time).
    bool waitingBackendResponse{false};
    bool backendFirstByteRecorded{false};
    std::chrono::steady_clock::time_point backendStart{};
    protocol::HttpResponseContext backendResp;
    std::shared_ptr<balancer::BackendConnectionPool::Lease> backendLease;
    bool closeAfterResponse{false};
    size_t backendBytesForwarded{0};
    std::string clientAcceptEncoding;
    bool forceStreamResponse{false};
    std::string backendResponseBuffer;
    bool backendResponseModeDecided{false};
    bool backendResponseConvert{false};
    size_t backendResponseBufLimit{8 * 1024 * 1024};
    std::string backendResponseStatusLine;
    int rewriteRuleIdx{-1};
    std::string mirrorMethod;
    std::string mirrorPath;
    std::string cacheKey;

    struct Http2Pending {
        proxy::network::InetAddress backendAddr{0};
        std::shared_ptr<balancer::BackendConnectionPool::Lease> lease;
        protocol::HttpResponseContext resp;
        std::string raw;
        std::string clientAcceptEncoding;
        int rewriteRuleIdx{-1};
        std::string clientIp;
        std::string mirrorMethod;
        std::string mirrorPath;
        bool firstByteRecorded{false};
        std::chrono::steady_clock::time_point start{};
    };
    std::unordered_map<uint32_t, std::shared_ptr<Http2Pending>> http2Pending;

    // Connection limit bookkeeping (user/service), acquired when first request is parsed.
    bool connLimitApplied{false};
    std::string userKey;
    std::string serviceKey;
};

} // namespace proxy
