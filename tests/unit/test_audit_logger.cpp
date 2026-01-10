#include "proxy/monitor/AuditLogger.h"
#include "proxy/common/Logger.h"

#include <cassert>
#include <cstdio>
#include <string>

int main() {
    proxy::common::Logger::Instance().SetLevel(proxy::common::LogLevel::ERROR);

    const char* path = "audit_logger_test.log";
    std::remove(path);

    {
        proxy::monitor::AuditLogger logger(path);
        logger.AppendLine("line1 a=b");
        logger.AppendLine("line2 x=y");
    }

    std::FILE* fp = std::fopen(path, "r");
    assert(fp);
    char buf[256];
    std::string all;
    while (std::fgets(buf, sizeof(buf), fp)) {
        all += buf;
    }
    std::fclose(fp);
    assert(all.find("line1 a=b\n") != std::string::npos);
    assert(all.find("line2 x=y\n") != std::string::npos);

    std::remove(path);
    return 0;
}

