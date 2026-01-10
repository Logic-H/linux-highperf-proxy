#include "proxy/ProxyServer.h"
#include "proxy/common/Logger.h"
#include "proxy/network/EventLoop.h"

#include <arpa/inet.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/ssl.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <thread>

using proxy::network::EventLoop;

namespace {

static const char* kTestCertPem =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDCTCCAfGgAwIBAgIUNZ0+r/Ha9PRgGduluMlnnukMBHUwDQYJKoZIhvcNAQEL\n"
    "BQAwFDESMBAGA1UEAwwJbG9jYWxob3N0MB4XDTI2MDEwODA4MzE1NloXDTI3MDEw\n"
    "ODA4MzE1NlowFDESMBAGA1UEAwwJbG9jYWxob3N0MIIBIjANBgkqhkiG9w0BAQEF\n"
    "AAOCAQ8AMIIBCgKCAQEAsBqVVj2FnJrY4qy49hvIg/G5xAW0IHg0RfKp8kED5kME\n"
    "q3m++Lh9ydk17r6abrSusSNkw14YBgfqLL2Kx+eukRlg4QWndG9rtJ4VyTauehb4\n"
    "3k3rcg3wZP2DVlr5L/LKHtaRjL90HQqIVO/+S5VV771JEVor0p9wbTR/En1cs2B+\n"
    "oF/IZEcVw4wIh5RHydI9+sG1/YT5edL4Gfo1QP3+PB56V2OKRLPu0R0sH0axIEN1\n"
    "pI4OUCC3z/Ny0i+x2Qi07kf2Ufl+iNtBm8HJqJRYC8FXU5A0LP2/oxGpgG/k4eVj\n"
    "wclNUqWsA3ZLfeISgCTdPF/H/XQjzmdXW28pdCc+FwIDAQABo1MwUTAdBgNVHQ4E\n"
    "FgQUQObSMz3Zvg+7Asy8wJcHdVawQVgwHwYDVR0jBBgwFoAUQObSMz3Zvg+7Asy8\n"
    "wJcHdVawQVgwDwYDVR0TAQH/BAUwAwEB/zANBgkqhkiG9w0BAQsFAAOCAQEAfq+z\n"
    "0fhslqHlnizVSMgS4XZ3Ge+gwd2wl+9PcsWOnTVVcTQTMfujkjDHSfYpcOnlL/bA\n"
    "F5trRC3H3QC283Zlbr5CGerE7OflYiWQ/e8wXnWSZtncSgECD8IcCWHEPXR933Gl\n"
    "L3lw80qFfH0mwLhVIBo5Bs5GIzzt+bgXzX0i8K/PcLf5gZjlPUfoW16xsM9098he\n"
    "cJ3MiaNo60ON54QW9PNmQldTKTWFVmmd8LPbkGtEGM8FIYiPsX4CK27X8umJffwh\n"
    "U0K6raxUKvBxkupzXm9lWl23yV56H2l3bp07qJVsMbwBuEAQebS865cauT22dYBg\n"
    "CVtLpEMMVx0YqHRr9w==\n"
    "-----END CERTIFICATE-----\n";

static const char* kTestKeyPem =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIIEvAIBADANBgkqhkiG9w0BAQEFAASCBKYwggSiAgEAAoIBAQCwGpVWPYWcmtji\n"
    "rLj2G8iD8bnEBbQgeDRF8qnyQQPmQwSreb74uH3J2TXuvpputK6xI2TDXhgGB+os\n"
    "vYrH566RGWDhBad0b2u0nhXJNq56FvjeTetyDfBk/YNWWvkv8soe1pGMv3QdCohU\n"
    "7/5LlVXvvUkRWivSn3BtNH8SfVyzYH6gX8hkRxXDjAiHlEfJ0j36wbX9hPl50vgZ\n"
    "+jVA/f48HnpXY4pEs+7RHSwfRrEgQ3Wkjg5QILfP83LSL7HZCLTuR/ZR+X6I20Gb\n"
    "wcmolFgLwVdTkDQs/b+jEamAb+Th5WPByU1SpawDdkt94hKAJN08X8f9dCPOZ1db\n"
    "byl0Jz4XAgMBAAECggEACPWW+9fgb2QD9Au/rqrYeaaTAfX4Cfsjl2Vt6P98fhZJ\n"
    "NJtIQsUAqwPno5l6cORr76vgFyJuJ76txH3xlcp2DhLBiKpbwtVdxOuSTyzaFXDC\n"
    "wUiZIrHXR595V+KWQ1NNMHTiSTmjdieiqrOVCaSqj2E6kX9cRh0BwHtsB+MkJiVz\n"
    "FY9j/+FrRejSjExXznPsbZS0Uvs9mupsNFqBsJOgpqSr6ZMyb6P7kRlAvUSVmGn5\n"
    "xeQUbttX7XbeNT/9ZthG/VQ6CGrNNq+wcW4DE2OtWrlQPkytLbKxO87bTwZAcsB3\n"
    "SV61ITykvffBP+qpcxFW4KsMFDuc93Eg+yKMxSP97QKBgQDafdC3lYkMvgRNbK9O\n"
    "SyuGlrKaPqxuIcypjkHdyOAOcB+YIraLMIPSNrR522bnTNEOypWEvoPht/OO5DJ0\n"
    "Dwu2uBQbQ12AxuZCw6c+edFxj1icknoK6/WyGCNehTJF52MArvnE8Ki2hRMOY0aU\n"
    "IhIHuJZoXd3pGzAdBcRpAOwKMwKBgQDOVe6QZm6wJVN8TQuhcEgAVMwPP8EIODKD\n"
    "AOWqcfpFgY90neCkEEGoC5oBwKH/J7Og0yuyeEaq6hhzvW1k1rV/FAvhacrfJTlk\n"
    "Cqk9BHxTmPgfpSOxSM6dr4PRYegwcWMpgbxht6zbfsb7E6CSRMkO9NbsMOZM+vm6\n"
    "TmBEKuPgjQKBgA/eg4JSAhpC0t0sv67YFtk3qNE7LBrzqYHHK1GIfX2O9zx9iPSj\n"
    "GW/zOTiAjwDNu4FvhVyW2c1TJwWB3YhAb4Fd7VftlKT9zUucGlGXV3cACleU6/hP\n"
    "GxFMX4VF91jMbYxnKQz8zcQz9iZdX4ryzoxxA7ug1bVQnDX/bTyTLJGxAoGAS9D1\n"
    "fOe2ilDc0VK6EFLikgSWio/hjPShUtO0kCGbVYjavn34ejBi/61rpHM00z2yWkjd\n"
    "yJE7XjfwTi7vkFr4i75A803dNotGAEWOlBYe3NTbbz1N1SmsifJhHEG+gHZZennq\n"
    "T3mfMXPvB3jZyoUt36oFgvog1W5u7o83/jRIUU0CgYAryszf5OSBEULoI/PTG5iB\n"
    "n+yhKpVYG+EYav1p2Q734XgJIE4daKnepcjfEVRrTKhAH2lzQPRvswSOcLepHn7h\n"
    "dZuEg2ahUdbYpj3shVXtrzaq/5U0NW7ldjjjr7NCp7hCCK4tiMBf+Fk1WdBmi2Mj\n"
    "aSqetiGgXKCCuFkTl4aIUg==\n"
    "-----END PRIVATE KEY-----\n";

static bool pollReadable(int fd, int timeoutMs) {
    pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN | POLLHUP | POLLERR;
    return ::poll(&pfd, 1, timeoutMs) == 1;
}

static void sendAll(int fd, const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
        ssize_t n = ::send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
        if (n <= 0) return;
        off += static_cast<size_t>(n);
    }
}

static int connectTo(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
        ::close(fd);
        return -1;
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

static std::optional<uint16_t> bindEphemeralTcpPort(int* listenFdOut) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return std::nullopt;
    int opt = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return std::nullopt;
    }
    if (::listen(fd, 32) != 0) {
        ::close(fd);
        return std::nullopt;
    }
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        ::close(fd);
        return std::nullopt;
    }
    const uint16_t port = ntohs(addr.sin_port);
    if (port == 0) {
        ::close(fd);
        return std::nullopt;
    }
    *listenFdOut = fd;
    return port;
}

static std::optional<uint16_t> reserveFreeTcpPort() {
    int fd = -1;
    auto port = bindEphemeralTcpPort(&fd);
    if (fd >= 0) ::close(fd);
    return port;
}

static std::string readAllWithTimeout(int fd, int timeoutMs) {
    const auto start = std::chrono::steady_clock::now();
    std::string out;
    while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() < timeoutMs) {
        if (!pollReadable(fd, 50)) continue;
        char buf[4096];
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        out.append(buf, buf + n);
    }
    return out;
}

static void backendHttpServer(int lfd, std::atomic<bool>* stop) {
    while (!stop->load()) {
        if (!pollReadable(lfd, 200)) continue;
        int cfd = ::accept(lfd, nullptr, nullptr);
        if (cfd < 0) continue;

        std::string in = readAllWithTimeout(cfd, 500);
        (void)in;
        std::string body = "OK";
        std::string resp = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK";
        sendAll(cfd, resp);
        ::shutdown(cfd, SHUT_RDWR);
        ::close(cfd);
    }
    ::close(lfd);
}

static std::string tmpFilePath(const std::string& name) {
    std::string p = "/tmp/";
    p += name;
    p += "_";
    p += std::to_string(::getpid());
    p += ".pem";
    return p;
}

} // namespace

int main() {
    ::signal(SIGPIPE, SIG_IGN);
    proxy::common::Logger::Instance().SetLevel(proxy::common::LogLevel::ERROR);

    // Write test cert/key.
    const std::string certPath = tmpFilePath("cert");
    const std::string keyPath = tmpFilePath("key");
    {
        FILE* f = std::fopen(certPath.c_str(), "w");
        assert(f);
        std::fwrite(kTestCertPem, 1, std::strlen(kTestCertPem), f);
        std::fclose(f);
        f = std::fopen(keyPath.c_str(), "w");
        assert(f);
        std::fwrite(kTestKeyPem, 1, std::strlen(kTestKeyPem), f);
        std::fclose(f);
    }

    // ACME challenge dir.
    const std::string acmeDir = std::string("/tmp/acme_chal_") + std::to_string(::getpid());
    ::mkdir(acmeDir.c_str(), 0700);
    const std::string token = "token123";
    const std::string tokenBody = "token123.thumbprint";
    {
        const std::string p = acmeDir + "/" + token;
        FILE* f = std::fopen(p.c_str(), "w");
        assert(f);
        std::fwrite(tokenBody.data(), 1, tokenBody.size(), f);
        std::fclose(f);
    }

    int bfd = -1;
    const auto bportOpt = bindEphemeralTcpPort(&bfd);
    const auto proxyPortOpt = reserveFreeTcpPort();
    assert(bportOpt.has_value());
    assert(proxyPortOpt.has_value());
    const uint16_t bport = *bportOpt;
    const uint16_t proxyPort = *proxyPortOpt;

    std::atomic<bool> stop{false};
    std::thread bt([&]() { backendHttpServer(bfd, &stop); });

    EventLoop loop;
    proxy::ProxyServer server(&loop, proxyPort, "roundrobin", "TlsAcmeProxy");
    server.AddBackend("127.0.0.1", bport, 1);
    server.SetAcmeChallengeDir(acmeDir);
    assert(server.EnableTls(certPath, keyPath));
    server.Start();

    std::thread client([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // Plain HTTP should still work (ACME path).
        {
            int fd = connectTo(proxyPort);
            assert(fd >= 0);
            const std::string req = "GET /.well-known/acme-challenge/" + token + " HTTP/1.1\r\n"
                                    "Host: test\r\n"
                                    "Connection: close\r\n"
                                    "\r\n";
            sendAll(fd, req);
            const std::string resp = readAllWithTimeout(fd, 1500);
            ::close(fd);
            assert(resp.find("200 OK") != std::string::npos);
            assert(resp.find(tokenBody) != std::string::npos);
        }

        // TLS client request should be terminated and proxied to backend.
        {
            int fd = connectTo(proxyPort);
            assert(fd >= 0);
            SSL_CTX* cctx = SSL_CTX_new(TLS_client_method());
            assert(cctx);
            SSL_CTX_set_verify(cctx, SSL_VERIFY_NONE, nullptr);
            SSL* ssl = SSL_new(cctx);
            assert(ssl);
            SSL_set_fd(ssl, fd);
            assert(SSL_connect(ssl) == 1);

            const std::string req =
                "GET /hello HTTP/1.1\r\nHost: test\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n";
            assert(SSL_write(ssl, req.data(), static_cast<int>(req.size())) > 0);

            std::string out;
            const auto start = std::chrono::steady_clock::now();
            while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() < 2000) {
                if (!pollReadable(fd, 50)) continue;
                char buf[4096];
                int n = SSL_read(ssl, buf, sizeof(buf));
                if (n <= 0) break;
                out.append(buf, buf + n);
                if (out.find("\r\n\r\n") != std::string::npos && out.find("OK") != std::string::npos) break;
            }

            SSL_shutdown(ssl);
            SSL_free(ssl);
            SSL_CTX_free(cctx);
            ::close(fd);

            assert(out.find("200 OK") != std::string::npos);
            assert(out.find("OK") != std::string::npos);
        }

        loop.QueueInLoop([&]() { loop.Quit(); });
    });

    loop.Loop();
    client.join();
    stop.store(true);
    bt.join();

    ::unlink(certPath.c_str());
    ::unlink(keyPath.c_str());
    return 0;
}
