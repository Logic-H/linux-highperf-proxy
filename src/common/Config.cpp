#include "proxy/common/Config.h"
#include "proxy/common/Logger.h"
#include <fstream>
#include <algorithm>
#include <sstream>

namespace proxy {
namespace common {

Config& Config::Instance() {
    static Config instance;
    return instance;
}

std::string Config::Trim(const std::string& s) {
    auto start = std::find_if_not(s.begin(), s.end(), isspace);
    auto end = std::find_if_not(s.rbegin(), s.rend(), isspace).base();
    return (start < end) ? std::string(start, end) : std::string();
}

bool Config::Load(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        LOG_ERROR << "Failed to open config file: " << filename;
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    settings_.clear();
    loadedFilename_ = filename;

    std::string line, section = "global";
    while (std::getline(file, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;

        if (line[0] == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            continue;
        }

        auto delimiterPos = line.find('=');
        if (delimiterPos != std::string::npos) {
            std::string key = Trim(line.substr(0, delimiterPos));
            std::string value = Trim(line.substr(delimiterPos + 1));
            settings_[section][key] = value;
        }
    }

    LOG_INFO << "Loaded config file: " << filename;
    return true;
}

bool Config::LoadFromString(const std::string& iniText) {
    std::istringstream in(iniText);
    if (!in.good()) return false;

    std::map<std::string, std::map<std::string, std::string>> parsed;
    std::string line, section = "global";
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;

        if (line[0] == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            continue;
        }

        auto delimiterPos = line.find('=');
        if (delimiterPos != std::string::npos) {
            std::string key = Trim(line.substr(0, delimiterPos));
            std::string value = Trim(line.substr(delimiterPos + 1));
            if (!key.empty()) parsed[section][key] = value;
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        settings_ = std::move(parsed);
    }
    return true;
}

bool Config::Save(const std::string& filename) {
    std::string outName = filename;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (outName.empty()) outName = loadedFilename_;
    }
    if (outName.empty()) return false;

    std::map<std::string, std::map<std::string, std::string>> snap;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snap = settings_;
        loadedFilename_ = outName;
    }

    std::ofstream f(outName, std::ios::trunc);
    if (!f.is_open()) return false;

    // Write [global] first if present.
    auto writeSection = [&](const std::string& section, const std::map<std::string, std::string>& kv) {
        f << "[" << section << "]\n";
        for (const auto& it : kv) {
            f << it.first << " = " << it.second << "\n";
        }
        f << "\n";
    };

    auto itg = snap.find("global");
    if (itg != snap.end()) {
        writeSection("global", itg->second);
        snap.erase(itg);
    }
    for (const auto& s : snap) {
        writeSection(s.first, s.second);
    }
    f.flush();
    return true;
}

void Config::SetString(const std::string& section, const std::string& key, const std::string& value) {
    if (section.empty() || key.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    settings_[section][key] = value;
}

void Config::DeleteKey(const std::string& section, const std::string& key) {
    if (section.empty() || key.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    auto sit = settings_.find(section);
    if (sit == settings_.end()) return;
    sit->second.erase(key);
}

void Config::DeleteSection(const std::string& section) {
    if (section.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    settings_.erase(section);
}

std::optional<std::string> Config::LoadedFilename() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (loadedFilename_.empty()) return std::nullopt;
    return loadedFilename_;
}

std::map<std::string, std::map<std::string, std::string>> Config::GetAll() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return settings_;
}

std::string Config::DumpIni() const {
    std::map<std::string, std::map<std::string, std::string>> snap;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snap = settings_;
    }

    std::ostringstream f;
    auto writeSection = [&](const std::string& section, const std::map<std::string, std::string>& kv) {
        f << "[" << section << "]\n";
        for (const auto& it : kv) {
            f << it.first << " = " << it.second << "\n";
        }
        f << "\n";
    };

    auto itg = snap.find("global");
    if (itg != snap.end()) {
        writeSection("global", itg->second);
        snap.erase(itg);
    }
    for (const auto& s : snap) {
        writeSection(s.first, s.second);
    }
    return f.str();
}

std::string Config::GetString(const std::string& section, const std::string& key, const std::string& defaultVal) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (settings_.count(section) && settings_[section].count(key)) {
        return settings_[section][key];
    }
    return defaultVal;
}

int Config::GetInt(const std::string& section, const std::string& key, int defaultVal) {
    std::string val = GetString(section, key, "");
    if (val.empty()) return defaultVal;
    try {
        return std::stoi(val);
    } catch (...) {
        return defaultVal;
    }
}

double Config::GetDouble(const std::string& section, const std::string& key, double defaultVal) {
    std::string val = GetString(section, key, "");
    if (val.empty()) return defaultVal;
    try {
        return std::stod(val);
    } catch (...) {
        return defaultVal;
    }
}

std::vector<Config::BackendConf> Config::GetBackends() {
    std::vector<BackendConf> result;
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Look for sections like [backend:1], [backend:2], etc.
    for (auto const& [section, keys] : settings_) {
        if (section.find("backend") == 0) {
            BackendConf conf;
            if (keys.count("ip") && keys.count("port")) {
                conf.ip = keys.at("ip");
                try {
                    conf.port = static_cast<uint16_t>(std::stoi(keys.at("port")));
                } catch (...) {
                    continue;
                }
                try {
                    conf.weight = keys.count("weight") ? std::stoi(keys.at("weight")) : 1;
                } catch (...) {
                    conf.weight = 1;
                }

                try {
                    if (keys.count("queue_len")) conf.queueLen = std::stoi(keys.at("queue_len"));
                } catch (...) {
                    conf.queueLen = -1;
                }
                try {
                    if (keys.count("gpu_util")) conf.gpuUtil01 = std::stod(keys.at("gpu_util"));
                } catch (...) {
                    conf.gpuUtil01 = -1.0;
                }
                try {
                    if (keys.count("vram_used_mb")) conf.vramUsedMb = std::stoi(keys.at("vram_used_mb"));
                } catch (...) {
                    conf.vramUsedMb = 0;
                }
                try {
                    if (keys.count("vram_total_mb")) conf.vramTotalMb = std::stoi(keys.at("vram_total_mb"));
                } catch (...) {
                    conf.vramTotalMb = 0;
                }
                result.push_back(conf);
            }
        }
    }
    return result;
}

std::vector<std::pair<std::string, std::map<std::string, std::string>>> Config::GetSectionsWithPrefix(const std::string& prefix) {
    std::vector<std::pair<std::string, std::map<std::string, std::string>>> out;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& kv : settings_) {
        const auto& section = kv.first;
        if (section.rfind(prefix, 0) != 0) continue;
        out.push_back({section, kv.second});
    }
    return out;
}

} // namespace common
} // namespace proxy
