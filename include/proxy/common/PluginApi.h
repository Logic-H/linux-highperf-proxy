#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Minimal stable C ABI for proxy plugins.
// This is intentionally small: enough to demonstrate "plugin architecture" without forcing
// the core server to expose internal C++ types.

#define PROXY_PLUGIN_API_VERSION 1

enum proxy_plugin_log_level {
    PROXY_PLUGIN_LOG_DEBUG = 0,
    PROXY_PLUGIN_LOG_INFO = 1,
    PROXY_PLUGIN_LOG_WARN = 2,
    PROXY_PLUGIN_LOG_ERROR = 3,
};

typedef struct proxy_plugin_host_v1 {
    int api_version;  // must be PROXY_PLUGIN_API_VERSION
    void (*log)(int level, const char* msg);
} proxy_plugin_host_v1;

typedef struct proxy_plugin_http_request_v1 {
    const char* method;
    const char* path;
    const char* query;
    const char* client_ip;
    const char* body;
    size_t body_len;
} proxy_plugin_http_request_v1;

typedef struct proxy_plugin_http_response_v1 {
    // If status <= 0, treated as "not handled".
    int status;
    const char* content_type;   // optional (defaults to text/plain)
    const char* extra_headers;  // optional raw header lines, e.g. "X-Foo: bar\r\nX-Bar: baz\r\n"
    const char* body;           // optional
    size_t body_len;
    void (*free_body)(const char* p);  // optional
} proxy_plugin_http_response_v1;

typedef struct proxy_plugin_v1 {
    int api_version;  // must be PROXY_PLUGIN_API_VERSION
    const char* name;
    int (*init)(const proxy_plugin_host_v1* host);  // return 0 on success
    void (*shutdown)();
    // Return 1 if handled and filled resp (status/body). Return 0 to fall through to core.
    int (*on_http_request)(const proxy_plugin_http_request_v1* req, proxy_plugin_http_response_v1* resp);
} proxy_plugin_v1;

typedef const proxy_plugin_v1* (*proxy_plugin_get_v1_fn)();

#ifdef __cplusplus
}
#endif

