#pragma once

#include "proxy/protocol/HttpRequest.h"
#include "proxy/network/Buffer.h"

namespace proxy {
namespace protocol {

class HttpContext {
public:
    enum HttpRequestParseState {
        kExpectRequestLine,
        kExpectHeaders,
        kExpectBody,
        kGotAll,
    };

    HttpContext()
        : state_(kExpectRequestLine) {}

    // return false if some error
    bool parseRequest(proxy::network::Buffer* buf, std::chrono::system_clock::time_point receiveTime);

    bool gotAll() const { return state_ == kGotAll; }
    void reset() {
        state_ = kExpectRequestLine;
        HttpRequest dummy;
        request_.swap(dummy);
        chunked_ = false;
        contentLength_ = 0;
        bodyRemaining_ = 0;
        chunkSize_ = 0;
        expectingChunkSize_ = true;
    }

    const HttpRequest& request() const { return request_; }
    HttpRequest& request() { return request_; }

private:
    bool processRequestLine(const char* begin, const char* end);
    static bool IEquals(const std::string& a, const std::string& b);

    HttpRequestParseState state_;
    HttpRequest request_;

    // Body parsing state
    bool chunked_{false};
    size_t contentLength_{0};
    size_t bodyRemaining_{0};
    size_t chunkSize_{0};
    bool expectingChunkSize_{true};
};

} // namespace protocol
} // namespace proxy
