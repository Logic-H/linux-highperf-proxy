#include "proxy/common/Logger.h"

#include <iostream>
#include <chrono>
#include <iomanip>
#include <ctime>

namespace proxy {
namespace common {

// Helper to get formatted timestamp
std::string GetCurrentTime() {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::stringstream ss;
    ss << std::put_time(std::localtime(&in_time_t), "%Y-%m-%d %H:%M:%S");
    ss << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return ss.str();
}

// Helper for log level strings and colors
const char* LevelToString(LogLevel level) {
    switch (level) {
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO:  return "INFO ";
        case LogLevel::WARN:  return "WARN ";
        case LogLevel::ERROR: return "ERROR";
        case LogLevel::FATAL: return "FATAL";
        default: return "UNKNOWN";
    }
}

// ANSI Color codes
const char* LevelToColor(LogLevel level) {
    switch (level) {
        case LogLevel::DEBUG: return "\033[36m"; // Cyan
        case LogLevel::INFO:  return "\033[32m"; // Green
        case LogLevel::WARN:  return "\033[33m"; // Yellow
        case LogLevel::ERROR: return "\033[31m"; // Red
        case LogLevel::FATAL: return "\033[35m"; // Magenta
        default: return "\033[0m";
    }
}

Logger& Logger::Instance() {
    static Logger instance;
    return instance;
}

void Logger::SetLevel(LogLevel level) {
    std::lock_guard<std::mutex> lock(mutex_);
    level_ = level;
}

LogLevel Logger::ParseLevel(const std::string& levelStr) {
    if (levelStr == "DEBUG") return LogLevel::DEBUG;
    if (levelStr == "INFO") return LogLevel::INFO;
    if (levelStr == "WARN") return LogLevel::WARN;
    if (levelStr == "ERROR") return LogLevel::ERROR;
    if (levelStr == "FATAL") return LogLevel::FATAL;
    return LogLevel::INFO;
}

void Logger::Log(LogLevel level, const char* file, int line, const std::string& msg) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Format: [Time] [Level] [File:Line] Message
    std::cout << LevelToColor(level)
              << "[" << GetCurrentTime() << "] "
              << "[" << LevelToString(level) << "] "
              << "[" << file << ":" << line << "] "
              << msg 
              << "\033[0m" // Reset color
              << std::endl;
}

} // namespace common
} // namespace proxy
