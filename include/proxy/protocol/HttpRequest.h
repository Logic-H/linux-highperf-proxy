#pragma once

#include <string>
#include <map>
#include <assert.h>
#include <cstddef>

namespace proxy {
namespace protocol {

class HttpRequest {
public:
    enum Method {
        kInvalid, kGet, kPost, kHead, kPut, kDelete
    };
    
    enum Version {
        kUnknown, kHttp10, kHttp11
    };

    HttpRequest() : method_(kInvalid), version_(kUnknown) {}

    void setVersion(Version v) { version_ = v; }
    Version getVersion() const { return version_; }

    bool setMethod(const char* start, const char* end) {
        std::string m(start, end);
        if (m == "GET") method_ = kGet;
        else if (m == "POST") method_ = kPost;
        else if (m == "HEAD") method_ = kHead;
        else if (m == "PUT") method_ = kPut;
        else if (m == "DELETE") method_ = kDelete;
        else method_ = kInvalid;
        return method_ != kInvalid;
    }

    Method getMethod() const { return method_; }
    const char* methodString() const {
        switch(method_) {
            case kGet: return "GET";
            case kPost: return "POST";
            case kHead: return "HEAD";
            case kPut: return "PUT";
            case kDelete: return "DELETE";
            default: return "UNKNOWN";
        }
    }

    void setPath(const char* start, const char* end) {
        path_.assign(start, end);
    }
    const std::string& path() const { return path_; }

    void setQuery(const char* start, const char* end) {
        query_.assign(start, end);
    }
    const std::string& query() const { return query_; }

    void addHeader(const char* start, const char* colon, const char* end) {
        std::string field(start, colon);
        ++colon;
        while (colon < end && isspace(*colon)) {
            ++colon;
        }
        std::string value(colon, end);
        while (!value.empty() && isspace(value[value.size()-1])) {
            value.resize(value.size()-1);
        }
        headers_[field] = value;
    }

    std::string getHeader(const std::string& field) const {
        std::string result;
        auto it = headers_.find(field);
        if (it != headers_.end()) {
            result = it->second;
        }
        return result;
    }

    void setHeader(const std::string& field, const std::string& value) {
        headers_[field] = value;
    }

    void removeHeader(const std::string& field) {
        auto it = headers_.find(field);
        if (it != headers_.end()) headers_.erase(it);
    }

    void setHeaderCI(const std::string& field, const std::string& value) {
        for (auto it = headers_.begin(); it != headers_.end();) {
            if (iequals_(it->first, field)) {
                it = headers_.erase(it);
                continue;
            }
            ++it;
        }
        headers_[field] = value;
    }

    void removeHeaderCI(const std::string& field) {
        for (auto it = headers_.begin(); it != headers_.end();) {
            if (iequals_(it->first, field)) {
                it = headers_.erase(it);
                continue;
            }
            ++it;
        }
    }

    const std::map<std::string, std::string>& headers() const { return headers_; }

    void setBody(const std::string& body) { body_ = body; }
    void appendBody(const char* data, size_t len) { body_.append(data, len); }
    const std::string& body() const { return body_; }

    void swap(HttpRequest& that) {
        std::swap(method_, that.method_);
        std::swap(version_, that.version_);
        path_.swap(that.path_);
        query_.swap(that.query_);
        headers_.swap(that.headers_);
        body_.swap(that.body_);
    }

private:
    static bool iequals_(const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            char ca = a[i];
            char cb = b[i];
            if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
            if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
            if (ca != cb) return false;
        }
        return true;
    }

    Method method_;
    Version version_;
    std::string path_;
    std::string query_;
    std::map<std::string, std::string> headers_;
    std::string body_;
};

} // namespace protocol
} // namespace proxy
