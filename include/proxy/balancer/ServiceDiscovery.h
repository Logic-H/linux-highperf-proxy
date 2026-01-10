#pragma once

#include "proxy/balancer/BackendManager.h"
#include "proxy/network/EventLoop.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace proxy {
namespace balancer {

struct DiscoveredBackend {
    std::string ip;
    uint16_t port{0};
    int weight{1};
};

// Background service discovery (management plane).
// - Polls Consul/Etcd/K8s over HTTP and reconciles backends into BackendManager.
class ServiceDiscoveryManager {
public:
    struct Config {
        std::string provider{"off"}; // off|consul|etcd|k8s
        double intervalSec{5.0};
        double timeoutSec{2.0};
        int defaultWeight{1};

        // Consul
        std::string consulUrl{"http://127.0.0.1:8500"};
        std::string consulService{};
        bool consulPassingOnly{true};

        // Etcd (v3 kv range on a single key; value is expected to be "ip:port,ip:port")
        std::string etcdUrl{"http://127.0.0.1:2379"};
        std::string etcdKey{};

        // Kubernetes (Endpoints resource)
        std::string k8sUrl{};        // e.g. https://127.0.0.1:6443
        std::string k8sToken{};      // Bearer token (optional)
        std::string k8sNamespace{"default"};
        std::string k8sEndpoints{};  // endpoints name
        bool k8sInsecureSkipVerify{true}; // HTTPS not implemented; kept for config compatibility
    };

    ServiceDiscoveryManager(proxy::network::EventLoop* loop, BackendManager* backendManager, Config cfg);
    ~ServiceDiscoveryManager();

    void Start();
    void Stop();

    // Fetch once for current provider (used by tests / manual checks).
    // Returns true if fetch succeeded (even if result is empty).
    bool FetchOnce(std::vector<DiscoveredBackend>* out);

private:
    void ThreadMain();
    void ApplyDiscovery(const std::vector<DiscoveredBackend>& items);

    proxy::network::EventLoop* loop_{nullptr};
    BackendManager* backendManager_{nullptr};
    Config cfg_;
    std::atomic<bool> stop_{false};
    std::thread th_;
    std::unordered_set<std::string> managed_; // backend ids managed by discovery
};

} // namespace balancer
} // namespace proxy
