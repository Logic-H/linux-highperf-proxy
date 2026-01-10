#pragma once

#include "proxy/protocol/HttpRequest.h"
#include "proxy/protocol/Hpack.h"

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace proxy {
namespace protocol {

struct RewriteRule {
    // Match conditions
    std::string pathPrefix;
    HttpRequest::Method method{HttpRequest::kInvalid}; // kInvalid => any

    // Request modifications
    std::unordered_map<std::string, std::string> reqSetHeaders;
    std::vector<std::string> reqDelHeaders;
    std::vector<std::pair<std::string, std::string>> reqBodyReplaces; // old->new

    // Response modifications
    std::unordered_map<std::string, std::string> respSetHeaders;
    std::vector<std::string> respDelHeaders;
    std::vector<std::pair<std::string, std::string>> respBodyReplaces; // old->new

    bool HasResponseMutations() const {
        return !respSetHeaders.empty() || !respDelHeaders.empty() || !respBodyReplaces.empty();
    }
};

class RewriteEngine {
public:
    void SetRules(std::vector<RewriteRule> rules) { rules_ = std::move(rules); }
    const std::vector<RewriteRule>& rules() const { return rules_; }

    int MatchHttp1(const HttpRequest& req) const;
    int MatchHttp2(const std::string& method, const std::string& path) const;

    // Returns true if request was modified.
    bool ApplyRequestHttp1(int ruleIdx, HttpRequest* req) const;
    bool ApplyRequestHttp2(int ruleIdx, std::vector<Hpack::Header>* headers, std::string* body) const;

    // Apply response modifications (headers/body). Returns true if modified.
    bool ApplyResponse(int ruleIdx, std::vector<Hpack::Header>* headers, std::string* body) const;

private:
    static bool IEquals(const std::string& a, const std::string& b);
    static void ReplaceAll(std::string* s, const std::string& from, const std::string& to);
    static void SetHeader(std::vector<Hpack::Header>* headers, const std::string& name, const std::string& value);
    static void DelHeader(std::vector<Hpack::Header>* headers, const std::string& name);

    std::vector<RewriteRule> rules_;
};

} // namespace protocol
} // namespace proxy

