#pragma once

#include <cstdio>
#include <mutex>
#include <string>

namespace proxy {
namespace monitor {

// Minimal audit access log (append-only).
class AuditLogger {
public:
    explicit AuditLogger(const std::string& path);
    ~AuditLogger();

    // Thread-safe append one line (already formatted).
    void AppendLine(const std::string& line);

    const std::string& path() const { return path_; }

private:
    std::string path_;
    std::mutex mutex_;
    std::FILE* fp_{nullptr};
};

} // namespace monitor
} // namespace proxy

