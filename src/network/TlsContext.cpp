#include "proxy/network/TlsContext.h"
#include "proxy/common/Logger.h"

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <atomic>

namespace proxy {
namespace network {

TlsContext::TlsContext() {
    static std::atomic<bool> inited{false};
    bool expected = false;
    if (inited.compare_exchange_strong(expected, true)) {
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_ssl_algorithms();
    }
}

TlsContext::~TlsContext() {
    if (ctx_) {
        SSL_CTX_free(reinterpret_cast<SSL_CTX*>(ctx_));
        ctx_ = nullptr;
    }
}

bool TlsContext::InitServer(const std::string& certPemPath, const std::string& keyPemPath) {
    if (certPemPath.empty() || keyPemPath.empty()) return false;
    if (ctx_) {
        SSL_CTX_free(reinterpret_cast<SSL_CTX*>(ctx_));
        ctx_ = nullptr;
    }

    SSL_CTX* c = SSL_CTX_new(TLS_server_method());
    if (!c) {
        LOG_ERROR << "TLS: SSL_CTX_new failed";
        return false;
    }

    // Reasonable defaults.
    SSL_CTX_set_min_proto_version(c, TLS1_2_VERSION);
    SSL_CTX_set_options(c, SSL_OP_NO_COMPRESSION);

    if (SSL_CTX_use_certificate_chain_file(c, certPemPath.c_str()) != 1) {
        LOG_ERROR << "TLS: load cert failed: " << certPemPath;
        SSL_CTX_free(c);
        return false;
    }
    if (SSL_CTX_use_PrivateKey_file(c, keyPemPath.c_str(), SSL_FILETYPE_PEM) != 1) {
        LOG_ERROR << "TLS: load key failed: " << keyPemPath;
        SSL_CTX_free(c);
        return false;
    }
    if (SSL_CTX_check_private_key(c) != 1) {
        LOG_ERROR << "TLS: key does not match cert";
        SSL_CTX_free(c);
        return false;
    }

    ctx_ = reinterpret_cast<ssl_ctx_st*>(c);
    return true;
}

} // namespace network
} // namespace proxy
