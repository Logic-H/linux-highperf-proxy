#include "proxy/protocol/Cookie.h"
#include "proxy/common/Logger.h"

#include <cassert>

int main() {
    proxy::common::Logger::Instance().SetLevel(proxy::common::LogLevel::INFO);

    using proxy::protocol::GetCookieValue;

    assert(GetCookieValue("a=1; b=2; c=3", "b") == "2");
    assert(GetCookieValue("sid=abc123", "sid") == "abc123");
    assert(GetCookieValue(" sid = x y ; other=z ", "sid") == "x y");
    assert(GetCookieValue("a=1; b=2", "missing").empty());
    assert(GetCookieValue("", "a").empty());
    assert(GetCookieValue("a=1", "").empty());
    return 0;
}

