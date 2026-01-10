#include "proxy/protocol/RewriteRules.h"

#include <algorithm>

namespace proxy {
namespace protocol {

bool RewriteEngine::IEquals(const std::string& a, const std::string& b) {
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

void RewriteEngine::ReplaceAll(std::string* s, const std::string& from, const std::string& to) {
    if (!s) return;
    if (from.empty()) return;
    size_t pos = 0;
    while (true) {
        pos = s->find(from, pos);
        if (pos == std::string::npos) break;
        s->replace(pos, from.size(), to);
        pos += to.size();
    }
}

void RewriteEngine::SetHeader(std::vector<Hpack::Header>* headers, const std::string& name, const std::string& value) {
    if (!headers) return;
    for (auto& h : *headers) {
        if (IEquals(h.name, name)) {
            h.value = value;
            return;
        }
    }
    headers->push_back({name, value});
}

void RewriteEngine::DelHeader(std::vector<Hpack::Header>* headers, const std::string& name) {
    if (!headers) return;
    headers->erase(std::remove_if(headers->begin(), headers->end(),
                                  [&](const Hpack::Header& h) { return IEquals(h.name, name); }),
                   headers->end());
}

int RewriteEngine::MatchHttp1(const HttpRequest& req) const {
    for (size_t i = 0; i < rules_.size(); ++i) {
        const auto& r = rules_[i];
        if (!r.pathPrefix.empty()) {
            if (req.path().rfind(r.pathPrefix, 0) != 0) continue;
        }
        if (r.method != HttpRequest::kInvalid && r.method != req.getMethod()) continue;
        return static_cast<int>(i);
    }
    return -1;
}

int RewriteEngine::MatchHttp2(const std::string& method, const std::string& path) const {
    HttpRequest::Method m = HttpRequest::kInvalid;
    if (method == "GET") m = HttpRequest::kGet;
    else if (method == "POST") m = HttpRequest::kPost;
    else if (method == "HEAD") m = HttpRequest::kHead;
    else if (method == "PUT") m = HttpRequest::kPut;
    else if (method == "DELETE") m = HttpRequest::kDelete;

    for (size_t i = 0; i < rules_.size(); ++i) {
        const auto& r = rules_[i];
        if (!r.pathPrefix.empty()) {
            if (path.rfind(r.pathPrefix, 0) != 0) continue;
        }
        if (r.method != HttpRequest::kInvalid && r.method != m) continue;
        return static_cast<int>(i);
    }
    return -1;
}

bool RewriteEngine::ApplyRequestHttp1(int ruleIdx, HttpRequest* req) const {
    if (!req) return false;
    if (ruleIdx < 0 || static_cast<size_t>(ruleIdx) >= rules_.size()) return false;
    const auto& r = rules_[static_cast<size_t>(ruleIdx)];
    bool changed = false;
    for (const auto& kv : r.reqSetHeaders) {
        req->setHeaderCI(kv.first, kv.second);
        changed = true;
    }
    for (const auto& k : r.reqDelHeaders) {
        req->removeHeaderCI(k);
        changed = true;
    }
    if (!r.reqBodyReplaces.empty()) {
        std::string b = req->body();
        for (const auto& rp : r.reqBodyReplaces) {
            ReplaceAll(&b, rp.first, rp.second);
        }
        if (b != req->body()) {
            req->setBody(b);
            changed = true;
        }
    }
    return changed;
}

bool RewriteEngine::ApplyRequestHttp2(int ruleIdx, std::vector<Hpack::Header>* headers, std::string* body) const {
    if (ruleIdx < 0 || static_cast<size_t>(ruleIdx) >= rules_.size()) return false;
    const auto& r = rules_[static_cast<size_t>(ruleIdx)];
    bool changed = false;
    if (headers) {
        for (const auto& kv : r.reqSetHeaders) {
            SetHeader(headers, kv.first, kv.second);
            changed = true;
        }
        for (const auto& k : r.reqDelHeaders) {
            DelHeader(headers, k);
            changed = true;
        }
    }
    if (body && !r.reqBodyReplaces.empty()) {
        for (const auto& rp : r.reqBodyReplaces) {
            ReplaceAll(body, rp.first, rp.second);
        }
        changed = true;
    }
    return changed;
}

bool RewriteEngine::ApplyResponse(int ruleIdx, std::vector<Hpack::Header>* headers, std::string* body) const {
    if (ruleIdx < 0 || static_cast<size_t>(ruleIdx) >= rules_.size()) return false;
    const auto& r = rules_[static_cast<size_t>(ruleIdx)];
    bool changed = false;
    if (headers) {
        for (const auto& kv : r.respSetHeaders) {
            SetHeader(headers, kv.first, kv.second);
            changed = true;
        }
        for (const auto& k : r.respDelHeaders) {
            DelHeader(headers, k);
            changed = true;
        }
    }
    if (body && !r.respBodyReplaces.empty()) {
        for (const auto& rp : r.respBodyReplaces) {
            ReplaceAll(body, rp.first, rp.second);
        }
        changed = true;
    }
    return changed;
}

} // namespace protocol
} // namespace proxy
