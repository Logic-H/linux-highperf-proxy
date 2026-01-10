#include "proxy/protocol/HttpBatcher.h"
#include "proxy/ProxySessionContext.h"
#include "proxy/common/Logger.h"
#include "proxy/monitor/Stats.h"

#include <sys/timerfd.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>

namespace proxy {
namespace protocol {

static bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

static std::string trim(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    size_t j = s.size();
    while (j > i && std::isspace(static_cast<unsigned char>(s[j - 1]))) --j;
    return s.substr(i, j - i);
}

static std::string getHeaderCI(const proxy::protocol::HttpRequest& req, const std::string& name) {
    for (const auto& kv : req.headers()) {
        if (iequals(kv.first, name)) return kv.second;
    }
    return {};
}

HttpBatcher::HttpBatcher(proxy::network::EventLoop* loop,
                         balancer::BackendConnectionPool* backendPool,
                         balancer::BackendManager* backendManager,
                         Config cfg,
                         ResumeCallback resumeCb)
    : loop_(loop),
      backendPool_(backendPool),
      backendManager_(backendManager),
      cfg_(std::move(cfg)),
      resumeCb_(std::move(resumeCb)) {
}

bool HttpBatcher::looksLikeJson(const std::string& body) {
    size_t i = 0;
    while (i < body.size() && std::isspace(static_cast<unsigned char>(body[i]))) ++i;
    if (i >= body.size()) return false;
    return body[i] == '{' || body[i] == '[';
}

bool HttpBatcher::pathAllowed(const std::string& path) const {
    if (cfg_.paths.empty()) return true;
    for (const auto& p : cfg_.paths) {
        if (p == path) return true;
    }
    return false;
}

bool HttpBatcher::isEligible(const proxy::protocol::HttpRequest& req) const {
    if (!cfg_.enabled) return false;
    if (req.getMethod() != proxy::protocol::HttpRequest::kPost) return false;
    if (!pathAllowed(req.path())) return false;
    if (req.body().empty()) return false;
    if (req.body().size() > cfg_.maxBatchBytes) return false;

    if (cfg_.requireHeader) {
        const std::string v = getHeaderCI(req, cfg_.headerName);
        if (v.empty()) return false;
        if (v != "1" && v != "true" && v != "TRUE" && v != "yes" && v != "YES") return false;
    }

    const std::string ct = getHeaderCI(req, "Content-Type");
    if (!ct.empty()) {
        const std::string lct = [&]() {
            std::string out;
            out.reserve(ct.size());
            for (unsigned char c : ct) out.push_back(static_cast<char>(std::tolower(c)));
            return out;
        }();
        if (lct.find("application/json") == std::string::npos && lct.find("text/json") == std::string::npos) {
            return looksLikeJson(req.body());
        }
    }
    return looksLikeJson(req.body());
}

void HttpBatcher::armTimer(Group* g) {
    if (!g || g->timerfd >= 0) return;
    int fd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd < 0) return;
    g->timerfd = fd;
    g->timerCh = std::make_shared<proxy::network::Channel>(loop_, fd);

    const std::string key = g->key;
    g->timerCh->SetReadCallback([self = shared_from_this(), key](std::chrono::system_clock::time_point) {
        self->onTimerFired(key);
    });
    g->timerCh->EnableReading();

    itimerspec ts;
    std::memset(&ts, 0, sizeof(ts));
    const int ms = std::max(0, cfg_.windowMs);
    ts.it_value.tv_sec = ms / 1000;
    ts.it_value.tv_nsec = (ms % 1000) * 1000000L;
    ::timerfd_settime(fd, 0, &ts, nullptr);
}

void HttpBatcher::disarmTimer(Group* g) {
    if (!g) return;
    if (g->timerCh) {
        g->timerCh->DisableAll();
        g->timerCh->Remove();
        g->timerCh.reset();
    }
    if (g->timerfd >= 0) {
        ::close(g->timerfd);
        g->timerfd = -1;
    }
}

void HttpBatcher::onTimerFired(const std::string& groupKey) {
    auto it = groups_.find(groupKey);
    if (it == groups_.end()) return;
    Group& g = it->second;
    if (g.timerfd >= 0) {
        uint64_t one = 0;
        ::read(g.timerfd, &one, sizeof(one));
    }
    // Avoid destroying the timer Channel inside its own read callback (UB).
    // Defer flush to the next loop iteration.
    loop_->QueueInLoop([self = shared_from_this(), groupKey]() { self->flushGroup(groupKey); });
}

bool HttpBatcher::TryEnqueue(const proxy::network::TcpConnectionPtr& client,
                             const std::shared_ptr<ProxySessionContext>& ctx,
                             const proxy::protocol::HttpRequest& req,
                             const proxy::network::InetAddress& backendAddr,
                             const std::string& model,
                             bool closeAfterResponse,
                             std::chrono::steady_clock::time_point start) {
    if (!loop_ || !backendPool_ || !backendManager_) return false;
    if (!isEligible(req)) return false;

    const std::string route = req.path() + req.query();
    const std::string key = backendAddr.toIpPort() + "|" + route + "|model:" + model;

    auto& g = groups_[key];
    if (g.key.empty()) {
        g.key = key;
        g.backendAddr = backendAddr;
        g.route = route;
        g.model = model;
        g.items.clear();
        g.bytes = 0;
        g.flushing = false;
        g.timerfd = -1;
        g.timerCh.reset();
    }

    if (g.flushing) return false;

    if (g.items.size() + 1 > cfg_.maxBatchSize) {
        flushGroup(key);
        auto it = groups_.find(key);
        if (it == groups_.end()) return false;
        return TryEnqueue(client, ctx, req, backendAddr, model, closeAfterResponse, start);
    }

    const size_t addBytes = req.body().size();
    if (g.bytes + addBytes > cfg_.maxBatchBytes) {
        flushGroup(key);
        auto it = groups_.find(key);
        if (it == groups_.end()) return false;
        return TryEnqueue(client, ctx, req, backendAddr, model, closeAfterResponse, start);
    }

    Item it;
    it.client = client;
    it.ctx = ctx;
    it.closeAfterResponse = closeAfterResponse;
    it.body = req.body();
    it.start = start;
    g.items.push_back(std::move(it));
    g.bytes += addBytes;

    if (g.items.size() == 1) {
        armTimer(&g);
    }
    if (g.items.size() >= cfg_.maxBatchSize) {
        flushGroup(key);
    }
    return true;
}

static std::string make502(const std::string& msg, bool close) {
    std::string body = msg;
    std::string resp = "HTTP/1.1 502 Bad Gateway\r\n"
                       "Content-Type: text/plain\r\n"
                       "Content-Length: " +
                       std::to_string(body.size()) +
                       "\r\n"
                       "Connection: " +
                       std::string(close ? "close" : "keep-alive") +
                       "\r\n"
                       "\r\n" +
                       body;
    return resp;
}

static std::string make200Json(const std::string& json, bool close) {
    std::string body = json;
    std::string resp = "HTTP/1.1 200 OK\r\n"
                       "Content-Type: application/json\r\n"
                       "Content-Length: " +
                       std::to_string(body.size()) +
                       "\r\n"
                       "Connection: " +
                       std::string(close ? "close" : "keep-alive") +
                       "\r\n"
                       "\r\n" +
                       body;
    return resp;
}

void HttpBatcher::failGroup(Group& g, const std::string& resp, bool backendKeepAlive) {
    for (auto& it : g.items) {
        auto ctx = it.ctx.lock();
        if (it.client) {
            it.client->Send(resp);
            if (it.closeAfterResponse) it.client->Shutdown();
        }
        if (ctx) ctx->waitingBackendResponse = false;
        if (resumeCb_ && it.client) {
            resumeCb_(it.client);
        }
    }
    g.items.clear();
    g.bytes = 0;
    disarmTimer(&g);
    (void)backendKeepAlive;
}

bool HttpBatcher::splitJsonArrayItems(const std::string& body, std::vector<std::string>* outItems) {
    if (!outItems) return false;
    outItems->clear();
    std::string s = trim(body);
    if (s.size() < 2 || s.front() != '[' || s.back() != ']') return false;
    size_t i = 1;
    bool inStr = false;
    bool esc = false;
    int depth = 1;
    size_t start = i;
    while (i + 1 < s.size()) {
        const char c = s[i];
        if (inStr) {
            if (esc) {
                esc = false;
            } else if (c == '\\') {
                esc = true;
            } else if (c == '"') {
                inStr = false;
            }
            ++i;
            continue;
        }
        if (c == '"') {
            inStr = true;
            ++i;
            continue;
        }
        if (c == '[' || c == '{') depth++;
        else if (c == ']' || c == '}') depth--;

        if (depth == 1 && c == ',') {
            const std::string part = trim(s.substr(start, i - start));
            if (!part.empty()) outItems->push_back(part);
            start = i + 1;
        }
        ++i;
    }
    const std::string last = trim(s.substr(start, (s.size() - 1) - start));
    if (!last.empty()) outItems->push_back(last);
    return true;
}

bool HttpBatcher::decodeChunkedBody(const std::string& raw, std::string* outBody) {
    if (!outBody) return false;
    outBody->clear();
    size_t p = 0;
    while (p < raw.size()) {
        size_t lineEnd = raw.find("\r\n", p);
        if (lineEnd == std::string::npos) return false;
        std::string line = raw.substr(p, lineEnd - p);
        const size_t semi = line.find(';');
        if (semi != std::string::npos) line = line.substr(0, semi);
        line = trim(line);
        if (line.empty()) return false;
        char* endp = nullptr;
        const unsigned long long n = std::strtoull(line.c_str(), &endp, 16);
        if (endp == line.c_str()) return false;
        p = lineEnd + 2;
        const size_t chunkLen = static_cast<size_t>(n);
        if (chunkLen == 0) {
            return true;
        }
        if (p + chunkLen + 2 > raw.size()) return false;
        outBody->append(raw.data() + p, raw.data() + p + chunkLen);
        p += chunkLen;
        if (raw.compare(p, 2, "\r\n") != 0) return false;
        p += 2;
    }
    return false;
}

bool HttpBatcher::extractBodyFromRawHttp(const std::string& raw, std::string* outBody, bool* outChunked) {
    if (!outBody || !outChunked) return false;
    outBody->clear();
    *outChunked = false;
    const size_t hdrEnd = raw.find("\r\n\r\n");
    if (hdrEnd == std::string::npos) return false;
    const std::string hdr = raw.substr(0, hdrEnd + 4);
    std::string body = raw.substr(hdrEnd + 4);
    bool chunked = false;
    {
        std::istringstream iss(hdr);
        std::string line;
        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            const std::string k = trim(line.substr(0, colon));
            const std::string v = trim(line.substr(colon + 1));
            if (iequals(k, "Transfer-Encoding")) {
                std::string lv;
                for (unsigned char c : v) lv.push_back(static_cast<char>(std::tolower(c)));
                if (lv.find("chunked") != std::string::npos) chunked = true;
            }
        }
    }
    if (!chunked) {
        *outBody = std::move(body);
        *outChunked = false;
        return true;
    }
    std::string decoded;
    if (!decodeChunkedBody(body, &decoded)) return false;
    *outBody = std::move(decoded);
    *outChunked = true;
    return true;
}

void HttpBatcher::deliverGroupResponse(Group& g, int statusCode, const std::string& body, bool backendKeepAlive) {
    (void)backendKeepAlive;
    if (statusCode < 200 || statusCode >= 400) {
        const std::string resp = make502("batch backend status=" + std::to_string(statusCode) + "\n", true);
        failGroup(g, resp, false);
        return;
    }
    std::vector<std::string> parts;
    if (!splitJsonArrayItems(body, &parts) || parts.size() != g.items.size()) {
        const std::string resp = make502("batch response mismatch\n", true);
        failGroup(g, resp, false);
        return;
    }
    for (size_t i = 0; i < g.items.size(); ++i) {
        auto& it = g.items[i];
        auto ctx = it.ctx.lock();
        if (it.client) {
            it.client->Send(make200Json(parts[i], it.closeAfterResponse));
            if (it.closeAfterResponse) it.client->Shutdown();
        }
        if (ctx) ctx->waitingBackendResponse = false;
        if (resumeCb_ && it.client) {
            resumeCb_(it.client);
        }
    }
    g.items.clear();
    g.bytes = 0;
    disarmTimer(&g);
}

void HttpBatcher::flushGroup(const std::string& groupKey) {
    auto it = groups_.find(groupKey);
    if (it == groups_.end()) return;
    Group& g = it->second;
    if (g.items.empty()) return;
    if (g.flushing) return;
    g.flushing = true;
    disarmTimer(&g);

    // Build batched JSON array body.
    std::string body;
    body.reserve(g.bytes + g.items.size() * 2 + 2);
    body.push_back('[');
    for (size_t i = 0; i < g.items.size(); ++i) {
        if (i) body.push_back(',');
        body += g.items[i].body;
    }
    body.push_back(']');

    // Prepare backend request.
    std::string req;
    req.reserve(1024 + body.size());
    req += "POST " + g.route + " HTTP/1.1\r\n";
    req += "Host: " + g.backendAddr.toIpPort() + "\r\n";
    req += "Connection: Keep-Alive\r\n";
    req += "Content-Type: application/json\r\n";
    if (!g.model.empty()) req += "X-Model: " + g.model + "\r\n";
    req += "X-Batch-Size: " + std::to_string(g.items.size()) + "\r\n";
    req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    req += "\r\n";
    req += body;

    backendPool_->Acquire(loop_, g.backendAddr,
                          [self = shared_from_this(), groupKey, req](std::shared_ptr<balancer::BackendConnectionPool::Lease> lease) {
                              auto it = self->groups_.find(groupKey);
                              if (it == self->groups_.end()) {
                                  if (lease) lease->Release(false);
                                  return;
                              }
                              Group& g = it->second;
                              if (!lease || !lease->connection() || !lease->connection()->connected()) {
                                  self->failGroup(g, make502("batch connect failed\n", true), false);
                                  if (lease) lease->Release(false);
                                  g.flushing = false;
                                  return;
                              }

                              auto backendConn = lease->connection();
                              self->backendManager_->OnBackendConnectionStart(g.backendAddr);
                              auto respCtx = std::make_shared<proxy::protocol::HttpResponseContext>();
                              respCtx->reset();
                              auto raw = std::make_shared<std::string>();
                              raw->reserve(std::min(self->cfg_.maxResponseBytes, static_cast<size_t>(64 * 1024)));
                              auto firstByte = std::make_shared<bool>(false);
                              auto start = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
                              auto finished = std::make_shared<bool>(false);

                              auto finalize = [self, groupKey, lease, backendConn, respCtx, raw, finished, firstByte](bool closed) mutable {
                                  if (*finished) return;
                                  *finished = true;

                                  auto it = self->groups_.find(groupKey);
                                  if (it == self->groups_.end()) {
                                      if (lease) lease->Release(false);
                                      return;
                                  }
                                  Group& g = it->second;

                                  bool ok = false;
                                  std::string body;
                                  bool chunked = false;
                                  if (!raw->empty()) {
                                      ok = extractBodyFromRawHttp(*raw, &body, &chunked);
                                  }
                                  const int code = respCtx->statusCode();
                                  const bool keep = !closed && respCtx->keepAlive() && respCtx->gotAll();
                                  if (!ok) {
                                      self->failGroup(g, make502("batch bad response\n", true), keep);
                                  } else {
                                      self->deliverGroupResponse(g, code, body, keep);
                                  }
                                  self->backendManager_->OnBackendConnectionEnd(g.backendAddr);
                                  if (lease) lease->Release(keep);
                                  g.flushing = false;
                              };

                              backendConn->SetCloseCallback([finalize](const proxy::network::TcpConnectionPtr&) mutable {
                                  finalize(true);
                              });

                              backendConn->SetMessageCallback([self, groupKey, respCtx, raw, firstByte, start, finalize](
                                                                  const proxy::network::TcpConnectionPtr&,
                                                                  proxy::network::Buffer* b,
                                                                  std::chrono::system_clock::time_point) mutable {
                                  if (!b) return;
                                  const char* data = b->Peek();
                                  const size_t n = b->ReadableBytes();
                                  if (n == 0) return;
                                  if (!*firstByte) {
                                      *firstByte = true;
                                      const auto now = std::chrono::steady_clock::now();
                                      const auto ms = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(now - *start).count();
                                      auto it = self->groups_.find(groupKey);
                                      if (it != self->groups_.end()) self->backendManager_->RecordBackendResponseTimeMs(it->second.backendAddr, ms);
                                  }
                                  if (raw->size() + n > self->cfg_.maxResponseBytes) {
                                      b->RetrieveAll();
                                      finalize(false);
                                      return;
                                  }
                                  raw->append(data, data + n);
                                  respCtx->feed(data, n);
                                  b->RetrieveAll();
                                  if (respCtx->hasError()) {
                                      finalize(false);
                                      return;
                                  }
                                  if (respCtx->gotAll()) {
                                      finalize(false);
                                      return;
                                  }
                              });

                              backendConn->Send(req);
                          });
}

} // namespace protocol
} // namespace proxy
