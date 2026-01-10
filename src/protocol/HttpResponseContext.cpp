#include "proxy/protocol/HttpResponseContext.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace proxy {
namespace protocol {

static bool isWs(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static bool findCrlfInSpan(const char* data, size_t len, size_t* posOut) {
    if (len < 2) return false;
    for (size_t i = 0; i + 1 < len; ++i) {
        if (data[i] == '\r' && data[i + 1] == '\n') {
            *posOut = i;
            return true;
        }
    }
    return false;
}

static bool findDoubleCrlfInSpan(const char* data, size_t len, size_t* posOut) {
    if (len < 4) return false;
    for (size_t i = 0; i + 3 < len; ++i) {
        if (data[i] == '\r' && data[i + 1] == '\n' && data[i + 2] == '\r' && data[i + 3] == '\n') {
            *posOut = i;
            return true;
        }
    }
    return false;
}

std::string HttpResponseContext::ToLowerCopy(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) out.push_back(static_cast<char>(std::tolower(c)));
    return out;
}

bool HttpResponseContext::IEquals(const std::string& a, const std::string& b) {
    return ToLowerCopy(a) == ToLowerCopy(b);
}

bool HttpResponseContext::HeaderContainsTokenCI(const std::string& v, const std::string& token) {
    const std::string lv = ToLowerCopy(v);
    const std::string lt = ToLowerCopy(token);
    return lv.find(lt) != std::string::npos;
}

void HttpResponseContext::reset() {
    state_ = kExpectStatusLine;
    headerBuf_.clear();
    httpMajor_ = 1;
    httpMinor_ = 1;
    statusCode_ = 0;
    headers_.clear();
    chunked_ = false;
    contentLength_ = 0;
    bodyRemaining_ = 0;
    keepAlive_ = false;
    needsCloseToFinish_ = false;
    expectingChunkSize_ = true;
    chunkLineBuf_.clear();
    chunkRemaining_ = 0;
    chunkCrlfRemaining_ = 0;
    trailer_ = false;
    trailerBuf_.clear();
}

bool HttpResponseContext::parseHeaderBlock(const std::string& headerBlock) {
    headers_.clear();

    // Status line + headers separated by CRLF, ends with CRLFCRLF.
    size_t pos = 0;
    size_t lineEnd = headerBlock.find("\r\n", pos);
    if (lineEnd == std::string::npos) {
        state_ = kError;
        return false;
    }
    const std::string statusLine = headerBlock.substr(0, lineEnd);
    pos = lineEnd + 2;

    // HTTP/1.1 200 OK
    if (statusLine.rfind("HTTP/", 0) != 0) {
        state_ = kError;
        return false;
    }
    const size_t sp1 = statusLine.find(' ');
    if (sp1 == std::string::npos) {
        state_ = kError;
        return false;
    }
    const std::string ver = statusLine.substr(5, sp1 - 5);
    const size_t dot = ver.find('.');
    if (dot == std::string::npos) {
        state_ = kError;
        return false;
    }
    try {
        httpMajor_ = std::stoi(ver.substr(0, dot));
        httpMinor_ = std::stoi(ver.substr(dot + 1));
    } catch (...) {
        state_ = kError;
        return false;
    }
    const size_t sp2 = statusLine.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) {
        state_ = kError;
        return false;
    }
    try {
        statusCode_ = std::stoi(statusLine.substr(sp1 + 1, sp2 - (sp1 + 1)));
    } catch (...) {
        state_ = kError;
        return false;
    }

    // Headers
    while (pos < headerBlock.size()) {
        const size_t next = headerBlock.find("\r\n", pos);
        if (next == std::string::npos) break;
        if (next == pos) {
            pos += 2;
            break;
        }
        const std::string line = headerBlock.substr(pos, next - pos);
        pos = next + 2;
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        std::string val = line.substr(colon + 1);
        while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(val.begin());
        while (!val.empty() && (val.back() == ' ' || val.back() == '\t')) val.pop_back();
        headers_[std::move(key)] = std::move(val);
    }

    // Determine body framing and keep-alive.
    std::string te;
    std::string cl;
    std::string conn;
    for (const auto& kv : headers_) {
        if (IEquals(kv.first, "Transfer-Encoding")) te = kv.second;
        else if (IEquals(kv.first, "Content-Length")) cl = kv.second;
        else if (IEquals(kv.first, "Connection")) conn = kv.second;
    }

    chunked_ = (!te.empty() && HeaderContainsTokenCI(te, "chunked"));
    contentLength_ = 0;
    if (!cl.empty()) {
        try {
            long long n = std::stoll(cl);
            if (n >= 0) contentLength_ = static_cast<size_t>(n);
        } catch (...) {
            contentLength_ = 0;
        }
    }

    keepAlive_ = true;
    if (httpMajor_ == 1 && httpMinor_ == 0) {
        keepAlive_ = HeaderContainsTokenCI(conn, "keep-alive");
    } else {
        if (HeaderContainsTokenCI(conn, "close")) keepAlive_ = false;
    }

    if (chunked_) {
        expectingChunkSize_ = true;
        chunkLineBuf_.clear();
        chunkRemaining_ = 0;
        chunkCrlfRemaining_ = 0;
        trailer_ = false;
        trailerBuf_.clear();
        needsCloseToFinish_ = false;
    } else if (!cl.empty()) {
        bodyRemaining_ = contentLength_;
        needsCloseToFinish_ = false;
    } else {
        needsCloseToFinish_ = true;
        keepAlive_ = false;
    }

    state_ = kExpectBody;
    return true;
}

bool HttpResponseContext::consumeChunked(const char* data, size_t len, size_t* consumed) {
    *consumed = 0;
    while (*consumed < len) {
        const char* p = data + *consumed;
        const size_t avail = len - *consumed;

        if (trailer_) {
            // Look for CRLFCRLF to end trailers.
            size_t pos = 0;
            if (findDoubleCrlfInSpan(p, avail, &pos)) {
                // include the delimiter
                *consumed += (pos + 4);
                state_ = kGotAll;
                return true;
            }
            // buffer a small trailer prefix to allow boundary across calls
            trailerBuf_.append(p, std::min(avail, static_cast<size_t>(8192)));
            if (trailerBuf_.size() >= 4) {
                const size_t found = trailerBuf_.find("\r\n\r\n");
                if (found != std::string::npos) {
                    state_ = kGotAll;
                    // We cannot precisely map bytes from trailerBuf_ back to this feed; be conservative:
                    // response completion will be observed on next feed/close.
                    return true;
                }
                if (trailerBuf_.size() > 8192) trailerBuf_.erase(0, trailerBuf_.size() - 8192);
            }
            *consumed = len;
            return true;
        }

        if (expectingChunkSize_) {
            size_t pos = 0;
            if (findCrlfInSpan(p, avail, &pos)) {
                // size line may be split; accumulate into chunkLineBuf_
                chunkLineBuf_.append(p, pos);
                *consumed += (pos + 2);

                std::string line = chunkLineBuf_;
                chunkLineBuf_.clear();
                const size_t semi = line.find(';');
                if (semi != std::string::npos) line = line.substr(0, semi);
                while (!line.empty() && isWs(static_cast<unsigned char>(line.front()))) line.erase(line.begin());
                while (!line.empty() && isWs(static_cast<unsigned char>(line.back()))) line.pop_back();
                if (line.empty()) {
                    state_ = kError;
                    return false;
                }
                char* endp = nullptr;
                const unsigned long long n = std::strtoull(line.c_str(), &endp, 16);
                if (endp == line.c_str()) {
                    state_ = kError;
                    return false;
                }
                chunkRemaining_ = static_cast<size_t>(n);
                expectingChunkSize_ = false;
                if (chunkRemaining_ == 0) {
                    trailer_ = true;
                }
                continue;
            }
            // no CRLF yet
            chunkLineBuf_.append(p, avail);
            *consumed = len;
            return true;
        }

        if (chunkRemaining_ > 0) {
            const size_t take = std::min(chunkRemaining_, avail);
            chunkRemaining_ -= take;
            *consumed += take;
            if (chunkRemaining_ == 0) {
                chunkCrlfRemaining_ = 2;
            }
            continue;
        }

        if (chunkCrlfRemaining_ > 0) {
            const size_t take = std::min(chunkCrlfRemaining_, avail);
            // Validate CRLF if present
            if (take >= 1) {
                if (chunkCrlfRemaining_ == 2 && p[0] != '\r') {
                    state_ = kError;
                    return false;
                }
                if (take == 2 && p[1] != '\n') {
                    state_ = kError;
                    return false;
                }
                if (chunkCrlfRemaining_ == 1 && p[0] != '\n') {
                    state_ = kError;
                    return false;
                }
            }
            chunkCrlfRemaining_ -= take;
            *consumed += take;
            if (chunkCrlfRemaining_ == 0) {
                expectingChunkSize_ = true;
            }
            continue;
        }
    }
    return true;
}

bool HttpResponseContext::consumeBody(const char* data, size_t len, size_t* consumed) {
    if (chunked_) {
        return consumeChunked(data, len, consumed);
    }
    if (needsCloseToFinish_) {
        // Cannot finish without close; just consume everything.
        *consumed = len;
        return true;
    }
    const size_t take = std::min(bodyRemaining_, len);
    bodyRemaining_ -= take;
    *consumed = take;
    if (bodyRemaining_ == 0) state_ = kGotAll;
    return true;
}

bool HttpResponseContext::feed(const char* data, size_t len) {
    if (state_ == kError || state_ == kGotAll) return (state_ == kGotAll);
    if (!data || len == 0) return state_ == kGotAll;

    size_t off = 0;
    while (off < len && state_ != kError && state_ != kGotAll) {
        if (state_ == kExpectStatusLine || state_ == kExpectHeaders) {
            // Accumulate until CRLFCRLF.
            headerBuf_.append(data + off, len - off);
            size_t hdrPos = headerBuf_.find("\r\n\r\n");
            if (hdrPos == std::string::npos) {
                return false;
            }
            const size_t headerEnd = hdrPos + 4;
            const std::string headerBlock = headerBuf_.substr(0, headerEnd);
            std::string bodyPart;
            if (headerBuf_.size() > headerEnd) bodyPart = headerBuf_.substr(headerEnd);
            headerBuf_.clear();

            if (!parseHeaderBlock(headerBlock)) return false;

            // consume any body bytes that already arrived
            if (!bodyPart.empty()) {
                size_t consumed = 0;
                if (!consumeBody(bodyPart.data(), bodyPart.size(), &consumed)) return false;
                // extra bytes (pipelined) are ignored by this response parser
            }
            return state_ == kGotAll;
        }

        if (state_ == kExpectBody) {
            size_t consumed = 0;
            if (!consumeBody(data + off, len - off, &consumed)) return false;
            off += consumed;
            // For "read until close", consumeBody consumes all remaining; break.
            if (needsCloseToFinish_) break;
            continue;
        }
    }
    return state_ == kGotAll;
}

} // namespace protocol
} // namespace proxy

