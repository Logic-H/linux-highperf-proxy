#pragma once

#include <string>
#include <map>
#include <stdio.h>
#include <cstring>

#include "proxy/network/Buffer.h"

namespace proxy {
namespace protocol {

class HttpResponse {
public:
    enum HttpStatusCode {
        kUnknown,
        k200Ok = 200,
        k301MovedPermanently = 301,
        k400BadRequest = 400,
        k404NotFound = 404,
        k500InternalServerError = 500,
    };

    explicit HttpResponse(bool close)
        : statusCode_(kUnknown), closeConnection_(close) {}

    void setStatusCode(HttpStatusCode code) { statusCode_ = code; }
    void setStatusMessage(const std::string& message) { statusMessage_ = message; }
    void setCloseConnection(bool on) { closeConnection_ = on; }
    bool closeConnection() const { return closeConnection_; }
    void setContentType(const std::string& contentType) { addHeader("Content-Type", contentType); }
    
    void addHeader(const std::string& key, const std::string& value) {
        headers_[key] = value;
    }

    void setBody(const std::string& body) { body_ = body; }

    void appendToBuffer(proxy::network::Buffer* output) const {
        char buf[32];
        snprintf(buf, sizeof buf, "HTTP/1.1 %d ", statusCode_);
        output->Append(buf, strlen(buf));
        output->Append(statusMessage_);
        output->Append("\r\n");

        if (closeConnection_) {
            output->Append("Connection: close\r\n");
        } else {
            snprintf(buf, sizeof buf, "Content-Length: %zd\r\n", body_.size());
            output->Append(buf, strlen(buf));
            output->Append("Connection: Keep-Alive\r\n");
        }

        for (const auto& header : headers_) {
            output->Append(header.first);
            output->Append(": ");
            output->Append(header.second);
            output->Append("\r\n");
        }

        output->Append("\r\n");
        output->Append(body_);
    }

private:
    HttpStatusCode statusCode_;
    std::string statusMessage_;
    bool closeConnection_;
    std::map<std::string, std::string> headers_;
    std::string body_;
};

} // namespace protocol
} // namespace proxy
