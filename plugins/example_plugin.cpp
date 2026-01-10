#include "proxy/common/PluginApi.h"

#include <string>
#include <cstring>

static proxy_plugin_host_v1 g_host{};

static int ExampleInit(const proxy_plugin_host_v1* host) {
    if (!host || host->api_version != PROXY_PLUGIN_API_VERSION) return 1;
    g_host = *host;
    if (g_host.log) g_host.log(PROXY_PLUGIN_LOG_INFO, "example_plugin init ok");
    return 0;
}

static void ExampleShutdown() {
    if (g_host.log) g_host.log(PROXY_PLUGIN_LOG_INFO, "example_plugin shutdown");
}

static int OnHttp(const proxy_plugin_http_request_v1* req, proxy_plugin_http_response_v1* resp) {
    if (!req || !resp) return 0;
    if (!req->path) return 0;

    const std::string path(req->path);
    if (path == "/plugin/hello") {
        static const char body[] = "{\"ok\":true,\"plugin\":\"example\"}\n";
        resp->status = 200;
        resp->content_type = "application/json";
        resp->extra_headers = "X-Plugin: example\r\n";
        resp->body = body;
        resp->body_len = sizeof(body) - 1;
        resp->free_body = nullptr;
        return 1;
    }
    if (path == "/plugin/echo") {
        const char* b = (req->body && req->body_len > 0) ? req->body : "";
        resp->status = 200;
        resp->content_type = "application/octet-stream";
        resp->extra_headers = "X-Plugin: example\r\n";
        resp->body = b;
        resp->body_len = req->body_len;
        resp->free_body = nullptr;
        return 1;
    }
    return 0;
}

static const proxy_plugin_v1 kPlugin{
    PROXY_PLUGIN_API_VERSION,
    "example_plugin",
    &ExampleInit,
    &ExampleShutdown,
    &OnHttp,
};

extern "C" const proxy_plugin_v1* proxy_plugin_get_v1() {
    return &kPlugin;
}

