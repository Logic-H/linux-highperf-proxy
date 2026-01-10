#include "proxy/monitor/AuditLogger.h"
#include "proxy/common/Logger.h"

namespace proxy {
namespace monitor {

AuditLogger::AuditLogger(const std::string& path) : path_(path) {
    if (path_.empty()) return;
    fp_ = std::fopen(path_.c_str(), "a");
    if (!fp_) {
        LOG_ERROR << "AuditLogger fopen failed path=" << path_;
    }
}

AuditLogger::~AuditLogger() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fp_) {
        std::fflush(fp_);
        std::fclose(fp_);
        fp_ = nullptr;
    }
}

void AuditLogger::AppendLine(const std::string& line) {
    if (!fp_) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!fp_) return;
    std::fwrite(line.data(), 1, line.size(), fp_);
    std::fwrite("\n", 1, 1, fp_);
    std::fflush(fp_);
}

} // namespace monitor
} // namespace proxy

