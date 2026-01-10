#include "proxy/network/Buffer.h"
#include "proxy/protocol/HttpContext.h"
#include "proxy/common/Logger.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace proxy {
namespace protocol {

static std::string ToLowerCopy(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) out.push_back(static_cast<char>(std::tolower(c)));
    return out;
}

bool HttpContext::IEquals(const std::string& a, const std::string& b) {
    return ToLowerCopy(a) == ToLowerCopy(b);
}

bool HttpContext::processRequestLine(const char* begin, const char* end) {
    bool succeed = false;
    const char* start = begin;
    const char* space = std::find(start, end, ' ');
    if (space != end && request_.setMethod(start, space)) {
        start = space + 1;
        space = std::find(start, end, ' ');
        if (space != end) {
            const char* question = std::find(start, space, '?');
            if (question != space) {
                request_.setPath(start, question);
                request_.setQuery(question, space);
            } else {
                request_.setPath(start, space);
            }
            start = space + 1;
            succeed = end - start == 8 && std::equal(start, end - 1, "HTTP/1.");
            if (succeed) {
                if (*(end - 1) == '1') {
                    request_.setVersion(HttpRequest::kHttp11);
                } else if (*(end - 1) == '0') {
                    request_.setVersion(HttpRequest::kHttp10);
                } else {
                    succeed = false;
                }
            }
        }
    }
    return succeed;
}

// return false if any error
bool HttpContext::parseRequest(proxy::network::Buffer* buf, std::chrono::system_clock::time_point receiveTime) {
    (void)receiveTime;
    bool ok = true;
    bool hasMore = true;
    while (hasMore) {
        if (state_ == kExpectRequestLine) {
            const char* crlf = std::search(buf->Peek(), static_cast<const char*>(buf->BeginWrite()), "\r\n", "\r\n" + 2);
            if (crlf < buf->BeginWrite()) {
                ok = processRequestLine(buf->Peek(), crlf);
                if (ok) {
                    buf->Retrieve(crlf + 2 - buf->Peek());
                    state_ = kExpectHeaders;
                } else {
                    hasMore = false;
                }
            } else {
                hasMore = false;
            }
        } else if (state_ == kExpectHeaders) {
            const char* crlf = std::search(buf->Peek(), static_cast<const char*>(buf->BeginWrite()), "\r\n", "\r\n" + 2);
            if (crlf < buf->BeginWrite()) {
                const char* colon = std::find(buf->Peek(), crlf, ':');
                if (colon != crlf) {
                    request_.addHeader(buf->Peek(), colon, crlf);
                } else {
                    // empty line, end of headers
                    buf->Retrieve(crlf + 2 - buf->Peek());

                    chunked_ = false;
                    contentLength_ = 0;
                    bodyRemaining_ = 0;
                    chunkSize_ = 0;
                    expectingChunkSize_ = true;

                    // Detect body
                    const std::string te = request_.getHeader("Transfer-Encoding");
                    if (!te.empty() && ToLowerCopy(te).find("chunked") != std::string::npos) {
                        chunked_ = true;
                    } else {
                        const std::string cl = request_.getHeader("Content-Length");
                        if (!cl.empty()) {
                            char* endp = nullptr;
                            long long v = std::strtoll(cl.c_str(), &endp, 10);
                            if (endp == cl.c_str() || v < 0) {
                                ok = false;
                                hasMore = false;
                                break;
                            }
                            contentLength_ = static_cast<size_t>(v);
                            bodyRemaining_ = contentLength_;
                        }
                    }

                    if (chunked_ || bodyRemaining_ > 0) {
                        state_ = kExpectBody;
                    } else {
                        state_ = kGotAll;
                    }
                    hasMore = (state_ != kGotAll);
                    continue;
                }
                buf->Retrieve(crlf + 2 - buf->Peek());
            } else {
                hasMore = false;
            }
        } else if (state_ == kExpectBody) {
            if (chunked_) {
                // Parse chunk-size lines and chunk data.
                while (true) {
                    if (expectingChunkSize_) {
                        const char* crlf = std::search(buf->Peek(), static_cast<const char*>(buf->BeginWrite()), "\r\n", "\r\n" + 2);
                        if (crlf >= buf->BeginWrite()) {
                            hasMore = false;
                            break;
                        }
                        std::string line(buf->Peek(), crlf);
                        buf->Retrieve(crlf + 2 - buf->Peek());

                        // Strip chunk extensions.
                        auto semi = line.find(';');
                        if (semi != std::string::npos) line = line.substr(0, semi);
                        // Trim
                        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
                        size_t i = 0;
                        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
                        line = line.substr(i);

                        if (line.empty()) {
                            ok = false;
                            hasMore = false;
                            break;
                        }
                        char* endp = nullptr;
                        long long sz = std::strtoll(line.c_str(), &endp, 16);
                        if (endp == line.c_str() || sz < 0) {
                            ok = false;
                            hasMore = false;
                            break;
                        }
                        chunkSize_ = static_cast<size_t>(sz);
                        expectingChunkSize_ = false;
                        if (chunkSize_ == 0) {
                            // Consume trailers until CRLFCRLF (may be immediate).
                            if (buf->ReadableBytes() >= 2) {
                                const char* p2 = buf->Peek();
                                if (p2[0] == '\r' && p2[1] == '\n') {
                                    // Empty trailers: the final CRLF.
                                    buf->Retrieve(2);
                                    state_ = kGotAll;
                                    hasMore = false;
                                    break;
                                }
                            }
                            const char* p = buf->Peek();
                            const char* end = buf->BeginWrite();
                            const char* dbl = std::search(p, end, "\r\n\r\n", "\r\n\r\n" + 4);
                            if (dbl >= end) {
                                hasMore = false;
                                break;
                            }
                            buf->Retrieve(dbl + 4 - buf->Peek());
                            state_ = kGotAll;
                            hasMore = false;
                            break;
                        }
                        // proceed to read data
                    }

                    // Need chunkSize_ bytes + CRLF.
                    if (buf->ReadableBytes() < chunkSize_ + 2) {
                        hasMore = false;
                        break;
                    }
                    request_.appendBody(buf->Peek(), chunkSize_);
                    buf->Retrieve(chunkSize_);
                    // Expect CRLF after chunk data
                    if (buf->ReadableBytes() < 2) {
                        hasMore = false;
                        break;
                    }
                    const char* p = buf->Peek();
                    if (p[0] != '\r' || p[1] != '\n') {
                        ok = false;
                        hasMore = false;
                        break;
                    }
                    buf->Retrieve(2);
                    expectingChunkSize_ = true;
                }
            } else {
                if (bodyRemaining_ == 0) {
                    state_ = kGotAll;
                    hasMore = false;
                    continue;
                }
                const size_t n = std::min(bodyRemaining_, buf->ReadableBytes());
                if (n > 0) {
                    request_.appendBody(buf->Peek(), n);
                    buf->Retrieve(n);
                    bodyRemaining_ -= n;
                }
                if (bodyRemaining_ == 0) {
                    state_ = kGotAll;
                }
                hasMore = false;
            }
        }
    }
    return ok;
}

// Reset needs to clear body-related parsing state as well.
// (Implementing here keeps header minimal and avoids missing resets in callers.)

} // namespace protocol
} // namespace proxy
