#pragma once

#include <string>
#include <mutex>
#include <sstream>

namespace proxy {
namespace common {

enum class LogLevel {
    DEBUG,
    INFO,
    WARN,
    ERROR,
    FATAL
};

class Logger {
public:
    static Logger& Instance();

    void SetLevel(LogLevel level);
    LogLevel GetLevel() const { return level_; }
    LogLevel ParseLevel(const std::string& levelStr);
    void Log(LogLevel level, const char* file, int line, const std::string& msg);

private:
    Logger() = default;
    ~Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    LogLevel level_ = LogLevel::INFO;
    std::mutex mutex_;
};

// Stream wrapper to allow usage like: LOG_INFO << "Message " << 123;
class LogStream {
public:
    LogStream(LogLevel level, const char* file, int line) 
        : level_(level), file_(file), line_(line) {}
    
    ~LogStream() {
        Logger::Instance().Log(level_, file_, line_, ss_.str());
    }

    template <typename T>
    LogStream& operator<<(const T& val) {
        ss_ << val;
        return *this;
    }

private:
    LogLevel level_;
    const char* file_;
    int line_;
    std::stringstream ss_;
};

} // namespace common
} // namespace proxy

// Macros for easy usage
#define LOG_DEBUG \
    if (proxy::common::LogLevel::DEBUG >= proxy::common::Logger::Instance().GetLevel()) \
    proxy::common::LogStream(proxy::common::LogLevel::DEBUG, __FILE__, __LINE__)

#define LOG_INFO \
    if (proxy::common::LogLevel::INFO >= proxy::common::Logger::Instance().GetLevel()) \
    proxy::common::LogStream(proxy::common::LogLevel::INFO, __FILE__, __LINE__)

#define LOG_WARN \
    if (proxy::common::LogLevel::WARN >= proxy::common::Logger::Instance().GetLevel()) \
    proxy::common::LogStream(proxy::common::LogLevel::WARN, __FILE__, __LINE__)

#define LOG_ERROR \
    if (proxy::common::LogLevel::ERROR >= proxy::common::Logger::Instance().GetLevel()) \
    proxy::common::LogStream(proxy::common::LogLevel::ERROR, __FILE__, __LINE__)

#define LOG_FATAL \
    if (proxy::common::LogLevel::FATAL >= proxy::common::Logger::Instance().GetLevel()) \
    proxy::common::LogStream(proxy::common::LogLevel::FATAL, __FILE__, __LINE__)
