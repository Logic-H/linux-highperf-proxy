#pragma once

#include <cstddef>
#include <map>
#include <string>

namespace proxy {
namespace protocol {

// Minimal incremental HTTP/1.x response parser for proxying/connection pooling.
// - Supports Content-Length and Transfer-Encoding: chunked.
// - If neither is present, treats response as "read until close" (not poolable).
class HttpResponseContext {
public:
    enum ParseState { kExpectStatusLine, kExpectHeaders, kExpectBody, kGotAll, kError };

    // Feed bytes without copying the whole response.
    // Returns true if the response is complete after processing these bytes.
    bool feed(const char* data, size_t len);

    bool gotAll() const { return state_ == kGotAll; }
    bool hasError() const { return state_ == kError; }

    void reset();

    bool keepAlive() const { return keepAlive_; }
    bool needsCloseToFinish() const { return needsCloseToFinish_; }
    int statusCode() const { return statusCode_; }

private:
    static std::string ToLowerCopy(const std::string& s);
    static bool IEquals(const std::string& a, const std::string& b);
    static bool HeaderContainsTokenCI(const std::string& v, const std::string& token);

    bool parseHeaderBlock(const std::string& headerBlock);
    bool consumeBody(const char* data, size_t len, size_t* consumed);
    bool consumeChunked(const char* data, size_t len, size_t* consumed);

    ParseState state_{kExpectStatusLine};
    std::string headerBuf_;

    int httpMajor_{1};
    int httpMinor_{1};
    int statusCode_{0};

    std::map<std::string, std::string> headers_; // original case keys
    bool chunked_{false};
    size_t contentLength_{0};
    size_t bodyRemaining_{0};
    bool keepAlive_{false};
    bool needsCloseToFinish_{false};

    // chunked parsing
    bool expectingChunkSize_{true};
    std::string chunkLineBuf_;
    size_t chunkRemaining_{0};
    size_t chunkCrlfRemaining_{0}; // bytes of CRLF left to consume after a chunk
    bool trailer_{false};
    std::string trailerBuf_;
};

} // namespace protocol
} // namespace proxy
