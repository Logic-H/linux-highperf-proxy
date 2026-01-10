#pragma once

#include "proxy/common/noncopyable.h"

#include <string>

struct ssl_ctx_st;

namespace proxy {
namespace network {

class TlsContext : proxy::common::noncopyable {
public:
    TlsContext();
    ~TlsContext();

    bool InitServer(const std::string& certPemPath, const std::string& keyPemPath);
    ssl_ctx_st* ctx() const { return ctx_; }
    bool ok() const { return ctx_ != nullptr; }

private:
    ssl_ctx_st* ctx_{nullptr};
};

} // namespace network
} // namespace proxy

