#include "proxy/common/PluginManager.h"
#include "proxy/common/Logger.h"
#include "proxy/common/PluginApi.h"

#include <dlfcn.h>
#include <sstream>
#include <utility>

namespace proxy::common {

static void HostLog(int level, const char* msg) {
    if (!msg) return;
    switch (level) {
        case PROXY_PLUGIN_LOG_DEBUG:
            LOG_DEBUG << "[plugin] " << msg;
            break;
        case PROXY_PLUGIN_LOG_INFO:
            LOG_INFO << "[plugin] " << msg;
            break;
        case PROXY_PLUGIN_LOG_WARN:
            LOG_WARN << "[plugin] " << msg;
            break;
        case PROXY_PLUGIN_LOG_ERROR:
        default:
            LOG_ERROR << "[plugin] " << msg;
            break;
    }
}

struct PluginManager::Impl {
    struct Loaded {
        std::string path;
        void* handle{nullptr};
        const proxy_plugin_v1* api{nullptr};
    };

    Config cfg{};
    std::vector<Loaded> plugins;

    proxy_plugin_host_v1 host{
        PROXY_PLUGIN_API_VERSION,
        &HostLog,
    };

    bool AllowedPath(const std::string& path) const {
        if (cfg.httpPathPrefixes.empty()) return true;
        for (const auto& p : cfg.httpPathPrefixes) {
            if (p.empty()) continue;
            if (path.rfind(p, 0) == 0) return true;
        }
        return false;
    }
};

PluginManager::PluginManager() : impl_(std::make_unique<Impl>()) {}

PluginManager::~PluginManager() { UnloadAll(); }

bool PluginManager::LoadAll(const Config& cfg) {
    UnloadAll();
    impl_->cfg = cfg;
    if (!cfg.enabled) return true;
    bool ok = true;
    for (const auto& p : cfg.paths) {
        if (p.empty()) continue;
        void* h = ::dlopen(p.c_str(), RTLD_NOW);
        if (!h) {
            ok = false;
            LOG_ERROR << "Plugin dlopen failed: path=" << p << " err=" << (::dlerror() ? ::dlerror() : "");
            continue;
        }
        auto sym = ::dlsym(h, "proxy_plugin_get_v1");
        if (!sym) {
            ok = false;
            LOG_ERROR << "Plugin missing symbol proxy_plugin_get_v1: path=" << p;
            ::dlclose(h);
            continue;
        }
        auto getApi = reinterpret_cast<proxy_plugin_get_v1_fn>(sym);
        const proxy_plugin_v1* api = getApi ? getApi() : nullptr;
        if (!api || api->api_version != PROXY_PLUGIN_API_VERSION || !api->name) {
            ok = false;
            LOG_ERROR << "Plugin API mismatch: path=" << p;
            ::dlclose(h);
            continue;
        }
        if (api->init) {
            const int rc = api->init(&impl_->host);
            if (rc != 0) {
                ok = false;
                LOG_ERROR << "Plugin init failed: path=" << p << " name=" << api->name << " rc=" << rc;
                ::dlclose(h);
                continue;
            }
        }
        Impl::Loaded loaded;
        loaded.path = p;
        loaded.handle = h;
        loaded.api = api;
        impl_->plugins.push_back(std::move(loaded));
        LOG_INFO << "Plugin loaded: " << api->name << " from " << p;
    }
    return ok;
}

void PluginManager::UnloadAll() {
    if (!impl_) return;
    for (auto& p : impl_->plugins) {
        if (p.api && p.api->shutdown) {
            try {
                p.api->shutdown();
            } catch (...) {
            }
        }
        if (p.handle) {
            ::dlclose(p.handle);
            p.handle = nullptr;
        }
        p.api = nullptr;
    }
    impl_->plugins.clear();
}

bool PluginManager::DispatchHttp(const std::string& method,
                                 const std::string& path,
                                 const std::string& query,
                                 const std::string& clientIp,
                                 const std::string& body,
                                 std::string* outHttpResponse) {
    if (!outHttpResponse) return false;
    outHttpResponse->clear();
    if (!impl_->cfg.enabled) return false;
    if (!impl_->AllowedPath(path)) return false;
    if (impl_->plugins.empty()) return false;

    proxy_plugin_http_request_v1 req{};
    req.method = method.c_str();
    req.path = path.c_str();
    req.query = query.c_str();
    req.client_ip = clientIp.c_str();
    req.body = body.data();
    req.body_len = body.size();

    for (const auto& p : impl_->plugins) {
        if (!p.api || !p.api->on_http_request) continue;
        proxy_plugin_http_response_v1 resp{};
        const int handled = p.api->on_http_request(&req, &resp);
        if (!handled) continue;
        if (resp.status <= 0) continue;

        const int status = resp.status;
        const char* ct = resp.content_type ? resp.content_type : "text/plain; charset=utf-8";
        const char* bodyPtr = resp.body ? resp.body : "";
        const size_t bodyLen = resp.body ? resp.body_len : 0;
        const std::string extra = resp.extra_headers ? std::string(resp.extra_headers) : std::string();

        std::ostringstream oss;
        oss << "HTTP/1.1 " << status << " OK\r\n"
            << "Content-Type: " << ct << "\r\n"
            << "Content-Length: " << bodyLen << "\r\n"
            << "Connection: close\r\n";
        if (!extra.empty()) {
            oss << extra;
            if (extra.size() < 2 || extra.substr(extra.size() - 2) != "\r\n") oss << "\r\n";
        }
        oss << "\r\n";
        oss.write(bodyPtr, static_cast<std::streamsize>(bodyLen));
        *outHttpResponse = oss.str();

        if (resp.free_body && resp.body) {
            try {
                resp.free_body(resp.body);
            } catch (...) {
            }
        }
        return true;
    }
    return false;
}

size_t PluginManager::LoadedCount() const { return impl_ ? impl_->plugins.size() : 0; }

}  // namespace proxy::common

