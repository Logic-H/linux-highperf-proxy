#pragma once

#include <memory>
#include <string>
#include <vector>

namespace proxy::common {

class PluginManager {
public:
    struct Config {
        bool enabled{false};
        // .so plugin paths
        std::vector<std::string> paths;
        // Only dispatch requests whose path starts with any of these prefixes.
        // Empty => dispatch all paths (not recommended).
        std::vector<std::string> httpPathPrefixes{"/plugin"};
    };

    PluginManager();
    ~PluginManager();

    bool LoadAll(const Config& cfg);
    void UnloadAll();

    // Dispatch one HTTP request to plugins. If any plugin handles, returns true and
    // fills `outHttpResponse` with a complete HTTP/1.1 response (Connection: close).
    bool DispatchHttp(const std::string& method,
                      const std::string& path,
                      const std::string& query,
                      const std::string& clientIp,
                      const std::string& body,
                      std::string* outHttpResponse);

    size_t LoadedCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace proxy::common

