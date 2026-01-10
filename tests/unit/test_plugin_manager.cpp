#include "proxy/common/PluginManager.h"

#include <cassert>
#include <string>
#include <unistd.h>

static bool fileExists(const std::string& p) { return ::access(p.c_str(), R_OK) == 0; }

int main() {
    proxy::common::PluginManager pm;
    proxy::common::PluginManager::Config cfg;
    cfg.enabled = true;
    cfg.httpPathPrefixes = {"/plugin"};
    const std::string soPath = "build/plugins/libproxy_example_plugin.so";
    assert(fileExists(soPath));
    cfg.paths = {soPath};
    assert(pm.LoadAll(cfg));
    assert(pm.LoadedCount() == 1);

    // Non-matching path should not be handled.
    {
        std::string resp;
        bool handled = pm.DispatchHttp("GET", "/not_plugin", "", "127.0.0.1", "", &resp);
        assert(!handled);
        assert(resp.empty());
    }

    // Plugin path should be handled.
    {
        std::string resp;
        bool handled = pm.DispatchHttp("GET", "/plugin/hello", "", "127.0.0.1", "", &resp);
        assert(handled);
        assert(resp.find("200") != std::string::npos);
        assert(resp.find("X-Plugin: example") != std::string::npos);
        assert(resp.find("\"plugin\":\"example\"") != std::string::npos);
    }
    return 0;
}

