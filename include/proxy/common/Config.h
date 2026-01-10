#pragma once

#include <string>
#include <map>
#include <vector>
#include <mutex>
#include <utility>
#include <optional>
#include "proxy/common/noncopyable.h"

namespace proxy {
namespace common {

class Config : noncopyable {
public:
    static Config& Instance();

    bool Load(const std::string& filename);
    // Parse INI text into in-memory settings (does not change loaded filename).
    bool LoadFromString(const std::string& iniText);
    bool Save(const std::string& filename = "");

    // Update config in-memory (does not auto-save).
    void SetString(const std::string& section, const std::string& key, const std::string& value);
    void DeleteKey(const std::string& section, const std::string& key);
    void DeleteSection(const std::string& section);

    // Returns the last loaded config filename if available.
    std::optional<std::string> LoadedFilename() const;

    // Returns a snapshot copy of all settings (thread-safe).
    std::map<std::string, std::map<std::string, std::string>> GetAll() const;
    // Dump current settings to INI text.
    std::string DumpIni() const;

    // Get value as string, return default if not found
    std::string GetString(const std::string& section, const std::string& key, const std::string& defaultVal = "");
    
    // Get value as int
    int GetInt(const std::string& section, const std::string& key, int defaultVal = 0);
    
    // Get value as double
    double GetDouble(const std::string& section, const std::string& key, double defaultVal = 0.0);

    // Get multiple values (e.g. backends)
    struct BackendConf {
        std::string ip;
        uint16_t port;
        int weight;
        int queueLen{-1};
        double gpuUtil01{-1.0};
        int vramUsedMb{0};
        int vramTotalMb{0};
    };
    std::vector<BackendConf> GetBackends();

    // Get sections whose name starts with prefix, returning (section_name, key->value) pairs.
    std::vector<std::pair<std::string, std::map<std::string, std::string>>> GetSectionsWithPrefix(const std::string& prefix);

private:
    Config() = default;
    std::string Trim(const std::string& s);

    mutable std::mutex mutex_;
    // map<section, map<key, value>>
    std::map<std::string, std::map<std::string, std::string>> settings_;
    std::string loadedFilename_;
};

} // namespace common
} // namespace proxy
