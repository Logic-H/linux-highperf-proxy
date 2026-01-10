#include "proxy/balancer/BackendManager.h"
#include "proxy/balancer/RoundRobinBalancer.h"
#include "proxy/balancer/ConsistentHashBalancer.h"
#include "proxy/balancer/LeastConnectionsBalancer.h"
#include "proxy/balancer/LeastQueueBalancer.h"
#include "proxy/balancer/ResponseTimeWeightedBalancer.h"
#include "proxy/balancer/GpuAwareBalancer.h"
#include "proxy/balancer/TcpHealthChecker.h"
#include "proxy/balancer/HttpHealthChecker.h"
#include "proxy/balancer/ScriptHealthChecker.h"
#include "proxy/common/Logger.h"
#include <sys/timerfd.h>
#include <unistd.h>
#include <cstring>
#include <algorithm>
#include <cmath>

namespace proxy {
namespace balancer {

BackendManager::BackendManager(proxy::network::EventLoop* loop, const std::string& strategy)
    : loop_(loop), checkIntervalSec_(5.0), checkTimerFd_(-1) {
    
    if (strategy == "hash") {
        balancer_ = std::make_shared<ConsistentHashBalancer>();
    } else if (strategy == "leastconn") {
        balancer_ = std::make_shared<LeastConnectionsBalancer>();
    } else if (strategy == "queue" || strategy == "leastqueue") {
        balancer_ = std::make_shared<LeastQueueBalancer>();
    } else if (strategy == "rtw") {
        balancer_ = std::make_shared<ResponseTimeWeightedBalancer>();
    } else if (strategy == "gpu") {
        balancer_ = std::make_shared<GpuAwareBalancer>();
    } else {
        balancer_ = std::make_shared<RoundRobinBalancer>();
    }
    
    healthChecker_ = std::make_shared<TcpHealthChecker>(loop, 2.0); // default 2s timeout
}

BackendManager::~BackendManager() {
    if (checkTimerChannel_) {
        checkTimerChannel_->DisableAll();
        checkTimerChannel_->Remove();
    }
    if (checkTimerFd_ >= 0) {
        ::close(checkTimerFd_);
    }
    if (aiTimerChannel_) {
        aiTimerChannel_->DisableAll();
        aiTimerChannel_->Remove();
    }
    if (aiTimerFd_ >= 0) {
        ::close(aiTimerFd_);
    }
}

std::vector<BackendManager::BackendSnapshot> BackendManager::GetBackendSnapshot() const {
    std::vector<BackendSnapshot> out;
    std::lock_guard<std::mutex> lock(mutex_);
    out.reserve(backends_.size());
    for (const auto& kv : backends_) {
        const auto& b = kv.second;
        BackendSnapshot s;
        s.id = b.ToId();
        s.healthy = b.healthy;
        s.online = b.online;
        s.aiReadyPresent = b.aiReadyPresent;
        s.aiReady = b.aiReady;
        s.weight = b.weight;
        s.baseWeight = b.baseWeight;
        s.activeConnections = b.activeConnections;
        s.ewmaResponseMs = b.ewmaResponseMs;
        s.failures = b.failures;
        s.successes = b.successes;
        {
            const double denom = static_cast<double>(b.failures + b.successes);
            s.errorRate = (denom > 0.0) ? (static_cast<double>(b.failures) / denom) : 0.0;
        }
        s.hasQueueLen = b.hasQueueLen;
        s.queueLen = b.queueLen;
        s.hasGpu = b.hasGpu;
        s.gpuUtil01 = b.gpuUtil01;
        s.vramUsedMb = b.vramUsedMb;
        s.vramTotalMb = b.vramTotalMb;
        s.hasModelLoaded = b.hasModelLoaded;
        s.modelLoaded = b.modelLoaded;
        s.hasModelName = b.hasModelName;
        s.modelName = b.modelName;
        s.hasModelVersion = b.hasModelVersion;
        s.modelVersion = b.modelVersion;
        out.push_back(std::move(s));
    }
    return out;
}

bool BackendManager::IsEligibleLocked(const BackendInfo& b) const {
    if (!b.online) return false;
    if (!b.healthy) return false;
    if (b.aiReadyPresent && !b.aiReady) return false;
    return true;
}

void BackendManager::RecomputeWeightLocked(BackendInfo& b) {
    if (!autoWeightAdjust_) {
        b.weight = std::max(1, b.baseWeight);
        return;
    }

    double factor = 1.0;
    if (b.hasQueueLen) {
        const double q = static_cast<double>(std::max(0, b.queueLen));
        factor *= 10.0 / (10.0 + q);
    }
    if (b.hasGpu) {
        double u = b.gpuUtil01;
        if (u < 0.0) u = 0.0;
        if (u > 1.0) u = 1.0;
        factor *= std::max(0.2, 1.0 - 0.8 * u);
    }
    if (b.ewmaResponseMs > 0.0) {
        factor *= 50.0 / (50.0 + b.ewmaResponseMs);
    }

    int w = static_cast<int>(std::lround(static_cast<double>(std::max(1, b.baseWeight)) * factor));
    w = std::clamp(w, 1, std::max(1, b.baseWeight));
    b.weight = w;
}

bool BackendManager::UpdateBackendMetrics(const std::string& id,
                                          int queueLen,
                                          double gpuUtil01,
                                          int vramUsedMb,
                                          int vramTotalMb) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = backends_.find(id);
    if (it == backends_.end()) return false;

    if (queueLen >= 0) {
        it->second.hasQueueLen = true;
        it->second.queueLen = queueLen;
        balancer_->RecordQueueLength(id, queueLen);
    }

    if (gpuUtil01 >= 0.0) {
        if (gpuUtil01 > 1.0) gpuUtil01 = 1.0;
        it->second.hasGpu = true;
        it->second.gpuUtil01 = gpuUtil01;
        it->second.vramUsedMb = vramUsedMb;
        it->second.vramTotalMb = vramTotalMb;
        balancer_->RecordGpuUtil(id, gpuUtil01, vramUsedMb, vramTotalMb);
    }
    const int oldW = it->second.weight;
    RecomputeWeightLocked(it->second);
    if (it->second.weight != oldW && IsEligibleLocked(it->second)) {
        balancer_->AddNode(id, it->second.weight);
    }
    return true;
}

void BackendManager::AddBackend(const std::string& ip, uint16_t port, int weight) {
    proxy::network::InetAddress addr(ip, port);
    std::string id;
    bool needWarmup = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
    BackendInfo info;
    info.addr = addr;
    info.baseWeight = std::max(1, weight);
    info.weight = info.baseWeight;
    info.healthy = true; // Assume healthy initially or false? Let's say true to start traffic immediately?
                         // Better false until check passes, but for responsiveness, maybe true?
                         // Requirement says: P99 < 10ms. Waiting for check might delay.
                         // Let's assume true, the health checker will correct it.
    info.online = true;

    if (warmupEnabled_ && warmupChecker_ && !warmupModel_.empty()) {
        // Gate traffic until warmup succeeds.
        info.aiReadyPresent = true;
        info.aiReady = false;
        info.hasModelLoaded = true;
        info.modelLoaded = false;
        info.hasModelName = true;
        info.modelName = warmupModel_;
        needWarmup = true;
    }
    
    id = info.ToId();
    backends_[id] = info;
    RebuildBalancer();
    }
    if (needWarmup) {
        StartWarmupIfNeeded(id, addr);
    }
}

void BackendManager::RemoveBackend(const std::string& ip, uint16_t port) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string id = proxy::network::InetAddress(ip, port).toIpPort();
    backends_.erase(id);
    RebuildBalancer();
}

bool BackendManager::RemoveBackendById(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = backends_.find(id);
    if (it == backends_.end()) return false;
    backends_.erase(it);
    RebuildBalancer();
    return true;
}

bool BackendManager::SetBackendOnline(const std::string& id, bool online) {
    proxy::network::InetAddress addr;
    bool needWarmup = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = backends_.find(id);
        if (it == backends_.end()) return false;
        if (it->second.online == online) return true;
        const bool oldEligible = IsEligibleLocked(it->second);
        it->second.online = online;
        addr = it->second.addr;

        // If bringing a backend online with warmup enabled, gate traffic until warmup succeeds.
        if (online && warmupEnabled_ && warmupChecker_ && !warmupModel_.empty()) {
            it->second.aiReadyPresent = true;
            it->second.aiReady = false;
            it->second.hasModelLoaded = true;
            it->second.modelLoaded = false;
            it->second.hasModelName = true;
            it->second.modelName = warmupModel_;
            needWarmup = true;
        }

        const bool newEligible = IsEligibleLocked(it->second);
        if (oldEligible != newEligible) {
            if (newEligible) balancer_->AddNode(id, it->second.weight);
            else balancer_->RemoveNode(id);
        } else if (!newEligible) {
            balancer_->RemoveNode(id);
        }
    }
    if (needWarmup) {
        StartWarmupIfNeeded(id, addr);
    }
    return true;
}

bool BackendManager::SetBackendBaseWeight(const std::string& id, int baseWeight) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = backends_.find(id);
    if (it == backends_.end()) return false;
    it->second.baseWeight = std::max(1, baseWeight);
    const int oldW = it->second.weight;
    RecomputeWeightLocked(it->second);
    if (it->second.weight != oldW && IsEligibleLocked(it->second)) {
        balancer_->AddNode(id, it->second.weight);
    }
    return true;
}

bool BackendManager::SetBackendLoadedModel(const std::string& id, const std::string& model, bool loaded) {
    return SetBackendLoadedModel(id, model, std::string(), loaded);
}

bool BackendManager::SetBackendLoadedModel(const std::string& id, const std::string& model, const std::string& version, bool loaded) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = backends_.find(id);
    if (it == backends_.end()) return false;

    BackendInfo& b = it->second;
    const bool oldEligible = IsEligibleLocked(b);

    b.aiReadyPresent = true;
    b.aiReady = loaded;
    b.hasModelLoaded = true;
    b.modelLoaded = loaded;
    if (!model.empty()) {
        b.hasModelName = true;
        b.modelName = model;
    }
    if (!version.empty()) {
        b.hasModelVersion = true;
        b.modelVersion = version;
    }

    if (!model.empty()) {
        auto mit = modelAffinity_.find(model);
        if (loaded) {
            if (mit == modelAffinity_.end() || mit->second.empty()) {
                modelAffinity_[model] = id;
            }
        } else {
            if (mit != modelAffinity_.end() && mit->second == id) {
                modelAffinity_.erase(mit);
            }
        }
    }

    if (!version.empty()) {
        const std::string mv = model.empty() ? (std::string("@") + version) : (model + "@" + version);
        auto mit = modelVersionAffinity_.find(mv);
        if (loaded) {
            if (mit == modelVersionAffinity_.end() || mit->second.empty()) {
                modelVersionAffinity_[mv] = id;
            }
        } else {
            if (mit != modelVersionAffinity_.end() && mit->second == id) {
                modelVersionAffinity_.erase(mit);
            }
        }
    }

    const bool newEligible = IsEligibleLocked(b);
    if (oldEligible != newEligible) {
        if (newEligible) balancer_->AddNode(id, b.weight);
        else balancer_->RemoveNode(id);
    }
    return true;
}

proxy::network::InetAddress BackendManager::SelectBackend(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string id = balancer_->GetNode(key);
    if (id.empty()) {
        return proxy::network::InetAddress(); // Invalid/Empty
    }
    
    auto it = backends_.find(id);
    if (it != backends_.end()) {
        return it->second.addr;
    }
    return proxy::network::InetAddress();
}

static uint64_t Fnv1a64(const std::string& s) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) {
        h ^= static_cast<uint64_t>(c);
        h *= 1099511628211ULL;
    }
    return h;
}

proxy::network::InetAddress BackendManager::SelectBackendForModel(const std::string& key, const std::string& model) {
    if (model.empty()) return SelectBackend(key);

    std::lock_guard<std::mutex> lock(mutex_);

    auto mit = modelAffinity_.find(model);
    if (mit != modelAffinity_.end() && !mit->second.empty()) {
        auto it = backends_.find(mit->second);
        if (it != backends_.end()) {
            const BackendInfo& b = it->second;
            const bool modelMatch = (!b.hasModelName) || (b.modelName == model);
            const bool modelReady = (!b.hasModelLoaded) || b.modelLoaded;
            if (IsEligibleLocked(b) && modelReady && modelMatch) {
                return b.addr;
            }
        }
        modelAffinity_.erase(mit);
    }

    // Prefer a backend that explicitly reports it has the target model loaded.
    std::vector<std::pair<std::string, int>> candidates; // id, weight
    candidates.reserve(backends_.size());
    for (const auto& kv : backends_) {
        const auto& b = kv.second;
        if (!IsEligibleLocked(b)) continue;
        if (b.hasModelLoaded && !b.modelLoaded) continue;
        if (!b.hasModelName) continue;
        if (b.modelName != model) continue;
        candidates.push_back({kv.first, std::max(1, b.weight)});
    }
    if (!candidates.empty()) {
        std::sort(candidates.begin(), candidates.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        long long sumW = 0;
        for (const auto& c : candidates) sumW += c.second;
        const uint64_t h = Fnv1a64(key);
        long long pick = static_cast<long long>(h % static_cast<uint64_t>(sumW));
        std::string chosenId;
        for (const auto& c : candidates) {
            pick -= c.second;
            if (pick < 0) {
                chosenId = c.first;
                break;
            }
        }
        if (!chosenId.empty()) {
            auto it = backends_.find(chosenId);
            if (it != backends_.end()) {
                modelAffinity_[model] = chosenId;
                return it->second.addr;
            }
        }
    }

    // Fallback: prefer a backend that doesn't explicitly report a different model loaded.
    std::vector<std::pair<std::string, int>> fallback; // id, weight
    fallback.reserve(backends_.size());
    for (const auto& kv : backends_) {
        const auto& b = kv.second;
        if (!IsEligibleLocked(b)) continue;
        const bool modelReady = (!b.hasModelLoaded) || b.modelLoaded;
        if (!modelReady) continue;
        if (b.hasModelName && b.modelName != model) continue;
        fallback.push_back({kv.first, std::max(1, b.weight)});
    }
    if (!fallback.empty()) {
        std::sort(fallback.begin(), fallback.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        long long sumW = 0;
        for (const auto& c : fallback) sumW += c.second;
        const uint64_t h = Fnv1a64(key);
        long long pick = static_cast<long long>(h % static_cast<uint64_t>(sumW));
        std::string chosenId;
        for (const auto& c : fallback) {
            pick -= c.second;
            if (pick < 0) {
                chosenId = c.first;
                break;
            }
        }
        if (!chosenId.empty()) {
            auto it = backends_.find(chosenId);
            if (it != backends_.end()) {
                modelAffinity_[model] = chosenId;
                return it->second.addr;
            }
        }
    }

    // Last resort: select by strategy and remember.
    std::string id = balancer_->GetNode(key);
    if (id.empty()) return proxy::network::InetAddress();
    auto it = backends_.find(id);
    if (it == backends_.end()) return proxy::network::InetAddress();
    modelAffinity_[model] = id;
    return it->second.addr;
}

proxy::network::InetAddress BackendManager::SelectBackendForModelVersion(const std::string& key,
                                                                         const std::string& model,
                                                                         const std::string& version) {
    if (version.empty()) {
        if (model.empty()) return SelectBackend(key);
        return SelectBackendForModel(key, model);
    }

    const std::string mv = model.empty() ? (std::string("@") + version) : (model + "@" + version);

    std::lock_guard<std::mutex> lock(mutex_);

    auto mit = modelVersionAffinity_.find(mv);
    if (mit != modelVersionAffinity_.end() && !mit->second.empty()) {
        auto it = backends_.find(mit->second);
        if (it != backends_.end()) {
            const BackendInfo& b = it->second;
            const bool modelMatch = model.empty() || (!b.hasModelName) || (b.modelName == model);
            const bool verMatch = (!b.hasModelVersion) || (b.modelVersion == version);
            const bool modelReady = (!b.hasModelLoaded) || b.modelLoaded;
            if (IsEligibleLocked(b) && modelReady && modelMatch && verMatch) {
                return b.addr;
            }
        }
        modelVersionAffinity_.erase(mit);
    }

    // Prefer a backend that explicitly reports it has the target model+version loaded.
    std::vector<std::pair<std::string, int>> candidates; // id, weight
    candidates.reserve(backends_.size());
    for (const auto& kv : backends_) {
        const auto& b = kv.second;
        if (!IsEligibleLocked(b)) continue;
        if (b.hasModelLoaded && !b.modelLoaded) continue;
        if (!b.hasModelVersion) continue;
        if (b.modelVersion != version) continue;
        if (!model.empty()) {
            if (!b.hasModelName) continue;
            if (b.modelName != model) continue;
        }
        candidates.push_back({kv.first, std::max(1, b.weight)});
    }
    if (!candidates.empty()) {
        std::sort(candidates.begin(), candidates.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        long long sumW = 0;
        for (const auto& c : candidates) sumW += c.second;
        const uint64_t h = Fnv1a64(key);
        long long pick = static_cast<long long>(h % static_cast<uint64_t>(sumW));
        std::string chosenId;
        for (const auto& c : candidates) {
            pick -= c.second;
            if (pick < 0) {
                chosenId = c.first;
                break;
            }
        }
        if (!chosenId.empty()) {
            auto it = backends_.find(chosenId);
            if (it != backends_.end()) {
                modelVersionAffinity_[mv] = chosenId;
                return it->second.addr;
            }
        }
    }

    // Fallback: prefer a backend that doesn't explicitly report a different version.
    std::vector<std::pair<std::string, int>> fallback; // id, weight
    fallback.reserve(backends_.size());
    for (const auto& kv : backends_) {
        const auto& b = kv.second;
        if (!IsEligibleLocked(b)) continue;
        const bool modelReady = (!b.hasModelLoaded) || b.modelLoaded;
        if (!modelReady) continue;
        if (b.hasModelVersion && b.modelVersion != version) continue;
        if (!model.empty() && b.hasModelName && b.modelName != model) continue;
        fallback.push_back({kv.first, std::max(1, b.weight)});
    }
    if (!fallback.empty()) {
        std::sort(fallback.begin(), fallback.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        long long sumW = 0;
        for (const auto& c : fallback) sumW += c.second;
        const uint64_t h = Fnv1a64(key);
        long long pick = static_cast<long long>(h % static_cast<uint64_t>(sumW));
        std::string chosenId;
        for (const auto& c : fallback) {
            pick -= c.second;
            if (pick < 0) {
                chosenId = c.first;
                break;
            }
        }
        if (!chosenId.empty()) {
            auto it = backends_.find(chosenId);
            if (it != backends_.end()) {
                modelVersionAffinity_[mv] = chosenId;
                return it->second.addr;
            }
        }
    }

    // Last resort: return empty (caller may decide to fail) to avoid routing to wrong version.
    return proxy::network::InetAddress();
}

void BackendManager::OnBackendConnectionStart(const proxy::network::InetAddress& addr) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = backends_.find(addr.toIpPort());
    if (it != backends_.end()) {
        it->second.activeConnections += 1;
        if (!it->second.hasQueueLen) {
            it->second.queueLen = it->second.activeConnections;
            balancer_->RecordQueueLength(addr.toIpPort(), it->second.queueLen);
        }
        const int oldW = it->second.weight;
        RecomputeWeightLocked(it->second);
    if (it->second.weight != oldW && IsEligibleLocked(it->second)) {
        balancer_->AddNode(addr.toIpPort(), it->second.weight);
    }
    }
    balancer_->OnConnectionStart(addr.toIpPort());
}

void BackendManager::OnBackendConnectionEnd(const proxy::network::InetAddress& addr) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = backends_.find(addr.toIpPort());
    if (it != backends_.end()) {
        if (it->second.activeConnections > 0) it->second.activeConnections -= 1;
        if (!it->second.hasQueueLen) {
            it->second.queueLen = it->second.activeConnections;
            balancer_->RecordQueueLength(addr.toIpPort(), it->second.queueLen);
        }
        const int oldW = it->second.weight;
        RecomputeWeightLocked(it->second);
        if (it->second.weight != oldW && IsEligibleLocked(it->second)) {
            balancer_->AddNode(addr.toIpPort(), it->second.weight);
        }
    }
    balancer_->OnConnectionEnd(addr.toIpPort());
}

void BackendManager::RecordBackendResponseTimeMs(const proxy::network::InetAddress& addr, double ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = backends_.find(addr.toIpPort());
    if (it != backends_.end()) {
        it->second.successes += 1;
        // Simple EWMA (alpha=0.2)
        const double alpha = 0.2;
        if (it->second.ewmaResponseMs <= 0.0) it->second.ewmaResponseMs = ms;
        else it->second.ewmaResponseMs = it->second.ewmaResponseMs * (1.0 - alpha) + ms * alpha;
        const int oldW = it->second.weight;
        RecomputeWeightLocked(it->second);
        if (it->second.weight != oldW && IsEligibleLocked(it->second)) {
            balancer_->AddNode(addr.toIpPort(), it->second.weight);
        }
    }
    balancer_->RecordResponseTimeMs(addr.toIpPort(), ms);
}

void BackendManager::ReportBackendFailure(const proxy::network::InetAddress& addr) {
    const std::string id = addr.toIpPort();
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = backends_.find(id);
    if (it == backends_.end()) return;
    it->second.failures += 1;
    if (!it->second.healthy) return;
    it->second.healthy = false;
    balancer_->RemoveNode(id);
    LOG_INFO << "Backend " << id << " marked DOWN by passive failure signal";
}

void BackendManager::ConfigureHealthCheck(const std::string& mode,
                                         double timeoutSec,
                                         const std::string& httpHost,
                                         const std::string& httpPath,
                                         const std::string& scriptCmd) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (mode == "off") {
        healthCheckEnabled_ = false;
        healthChecker_.reset();
        return;
    }
    healthCheckEnabled_ = true;
    if (mode == "http") {
        healthChecker_ = std::make_shared<HttpHealthChecker>(loop_, timeoutSec, httpHost, httpPath);
    } else if (mode == "script") {
        healthChecker_ = std::make_shared<ScriptHealthChecker>(loop_, timeoutSec, scriptCmd);
    } else {
        healthChecker_ = std::make_shared<TcpHealthChecker>(loop_, timeoutSec);
    }
}

void BackendManager::ConfigureAiServiceCheck(double timeoutSec,
                                             const std::string& httpHost,
                                             const std::string& httpPath) {
    std::lock_guard<std::mutex> lock(mutex_);
    aiTimeoutSec_ = timeoutSec;
    aiHttpHost_ = httpHost;
    aiHttpPath_ = httpPath;
    aiChecker_ = std::make_shared<AiServiceChecker>(loop_, aiTimeoutSec_, aiHttpHost_, aiHttpPath_);
}

void BackendManager::ConfigureWarmup(bool enable,
                                     const std::string& model,
                                     double timeoutSec,
                                     const std::string& httpHost,
                                     const std::string& httpPath) {
    std::lock_guard<std::mutex> lock(mutex_);
    warmupEnabled_ = enable;
    warmupModel_ = model;
    warmupTimeoutSec_ = timeoutSec;
    warmupHttpHost_ = httpHost;
    warmupHttpPath_ = httpPath;
    if (warmupEnabled_ && !warmupModel_.empty()) {
        warmupChecker_ = std::make_shared<WarmupChecker>(loop_, warmupTimeoutSec_, warmupHttpHost_, warmupHttpPath_);
    } else {
        warmupChecker_.reset();
    }
}

void BackendManager::StartWarmupIfNeeded(const std::string& id, const proxy::network::InetAddress& addr) {
    std::shared_ptr<WarmupChecker> checker;
    std::string model;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!warmupEnabled_ || !warmupChecker_ || warmupModel_.empty()) return;
        checker = warmupChecker_;
        model = warmupModel_;
    }
    checker->Warmup(addr, model, [this, id, model](bool ok, const proxy::network::InetAddress&) {
        if (!ok) {
            LOG_WARN << "Warmup failed for backend " << id << " model=" << model;
            return;
        }
        bool applied = this->SetBackendLoadedModel(id, model, true);
        if (applied) {
            LOG_INFO << "Warmup ok for backend " << id << " model=" << model;
        }
    });
}

void BackendManager::RebuildBalancer() {
    // Note: This simplistic rebuild is O(N). Efficient for small N (N < 100).
    // For 10k nodes, we need delta updates.
    // Balancer interface supports Add/Remove.
    
    // We should clear the balancer and re-add healthy nodes?
    // Current Balancer interface doesn't have Clear.
    // So we'll just recreate it or update it.
    // But Balancer implementation (RoundRobin) clears on Add? No.
    
    // Let's rely on Re-creating the balancer pointer for now if we want full consistency,
    // Or we iterate and Add/Remove.
    // To implement "Update", we probably need to know what's currently in balancer.
    // For now, let's assume we just keep adding/removing.
    
    // Actually, Balancer::AddNode/RemoveNode is fine.
    // But "Rebuild" implies we scan 'backends_' and sync 'balancer_'.
    
    // Simple Sync:
    // This is tricky without knowing what's in Balancer.
    // Let's assume we just call AddNode/RemoveNode when status changes.
    // But here 'RebuildBalancer' is called from AddBackend/RemoveBackend.
    
    // Correct approach:
    // When AddBackend -> AddNode(id, weight)
    // When RemoveBackend -> RemoveNode(id)
    // When HealthChange -> Add or Remove
    
    // So we don't need a full Rebuild function, just incremental updates.
    // However, AddBackend logic calls Rebuild. Let's fix that.
    
    // Wait, 'AddNode' implementation in RoundRobin just appends.
    // If we call AddNode for existing node, it might duplicate or update.
    // My RoundRobin implementation handles re-addition by removing first.
    
    for (const auto& kv : backends_) {
        const auto& backend = kv.second;
        if (IsEligibleLocked(backend)) {
            balancer_->AddNode(backend.ToId(), backend.weight);
        } else {
            balancer_->RemoveNode(backend.ToId());
        }
    }
}

void BackendManager::StartHealthCheck(double intervalSec) {
    checkIntervalSec_ = intervalSec;
    loop_->RunInLoop([this]() {
        if (!healthCheckEnabled_ || checkIntervalSec_ <= 0.0 || !healthChecker_) {
            if (checkTimerChannel_) {
                checkTimerChannel_->DisableAll();
                checkTimerChannel_->Remove();
                checkTimerChannel_.reset();
            }
            if (checkTimerFd_ >= 0) {
                ::close(checkTimerFd_);
                checkTimerFd_ = -1;
            }
            return;
        }

        if (checkTimerFd_ >= 0 && checkTimerChannel_) {
            ScheduleNextCheck();
            return;
        }

        checkTimerFd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (checkTimerFd_ < 0) {
            LOG_ERROR << "BackendManager timerfd error";
            return;
        }

        checkTimerChannel_ = std::make_shared<proxy::network::Channel>(loop_, checkTimerFd_);
        checkTimerChannel_->SetReadCallback([this](std::chrono::system_clock::time_point) {
            // Read timer to clear
            uint64_t one;
            ::read(checkTimerFd_, &one, sizeof one);
            RunHealthCheck();
        });
        checkTimerChannel_->EnableReading();
        
        ScheduleNextCheck();
    });
}

void BackendManager::StartAiServiceCheck(double intervalSec) {
    aiIntervalSec_ = intervalSec;
    loop_->RunInLoop([this]() {
        if (!aiChecker_) {
            aiChecker_ = std::make_shared<AiServiceChecker>(loop_, aiTimeoutSec_, aiHttpHost_, aiHttpPath_);
        }
        aiTimerFd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (aiTimerFd_ < 0) {
            LOG_ERROR << "BackendManager ai timerfd error";
            return;
        }
        aiTimerChannel_ = std::make_shared<proxy::network::Channel>(loop_, aiTimerFd_);
        aiTimerChannel_->SetReadCallback([this](std::chrono::system_clock::time_point) {
            uint64_t one;
            ::read(aiTimerFd_, &one, sizeof one);
            RunAiServiceCheck();
        });
        aiTimerChannel_->EnableReading();
        ScheduleNextAiCheck();
    });
}

void BackendManager::ScheduleNextCheck() {
    struct itimerspec howlong;
    bzero(&howlong, sizeof howlong);
    howlong.it_value.tv_sec = static_cast<time_t>(checkIntervalSec_);
    howlong.it_value.tv_nsec = static_cast<long>((checkIntervalSec_ - (long)checkIntervalSec_) * 1000000000);
    ::timerfd_settime(checkTimerFd_, 0, &howlong, NULL);
}

void BackendManager::ScheduleNextAiCheck() {
    if (aiTimerFd_ < 0) return;
    struct itimerspec howlong;
    bzero(&howlong, sizeof howlong);
    howlong.it_value.tv_sec = static_cast<time_t>(aiIntervalSec_);
    howlong.it_value.tv_nsec = static_cast<long>((aiIntervalSec_ - (long)aiIntervalSec_) * 1000000000);
    ::timerfd_settime(aiTimerFd_, 0, &howlong, NULL);
}

void BackendManager::RunHealthCheck() {
    if (!healthCheckEnabled_ || !healthChecker_ || checkIntervalSec_ <= 0.0) return;
    std::vector<std::pair<std::string, proxy::network::InetAddress>> targets;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        targets.reserve(backends_.size());
        for (const auto& kv : backends_) {
            targets.emplace_back(kv.first, kv.second.addr);
        }
    }

    for (const auto& t : targets) {
        const std::string id = t.first;
        const proxy::network::InetAddress addr = t.second;
        healthChecker_->Check(addr, [this, id](bool healthy, const proxy::network::InetAddress&) {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = backends_.find(id);
            if (it == backends_.end()) return;
            BackendInfo& b = it->second;
            const bool oldEligible = IsEligibleLocked(b);
            const bool oldHealthy = b.healthy;
            b.healthy = healthy;
            const bool newEligible = IsEligibleLocked(b);
            if (oldHealthy != healthy) {
                LOG_INFO << "Backend " << id << " health changed to " << (healthy ? "UP" : "DOWN");
            }
            if (oldEligible != newEligible) {
                if (newEligible) balancer_->AddNode(id, b.weight);
                else balancer_->RemoveNode(id);
            }
        });
    }
    ScheduleNextCheck();
}

void BackendManager::RunAiServiceCheck() {
    if (!aiChecker_) {
        ScheduleNextAiCheck();
        return;
    }

    std::vector<proxy::network::InetAddress> addrs;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        addrs.reserve(backends_.size());
        for (const auto& kv : backends_) {
            addrs.push_back(kv.second.addr);
        }
    }

    for (const auto& addr : addrs) {
        aiChecker_->Check(addr, [this](bool ok, const proxy::network::InetAddress& a, const AiServiceChecker::Result& r) {
            OnAiResult(ok, a, r);
        });
    }

    ScheduleNextAiCheck();
}

void BackendManager::OnAiResult(bool ok, const proxy::network::InetAddress& addr, const AiServiceChecker::Result& r) {
    const std::string id = addr.toIpPort();
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = backends_.find(id);
    if (it == backends_.end()) return;

    BackendInfo& b = it->second;
    const bool oldEligible = IsEligibleLocked(b);
    const bool oldAiReadyPresent = b.aiReadyPresent;
    const bool oldAiReady = b.aiReady;

    if (ok) {
        if (r.hasQueueLen) {
            b.hasQueueLen = true;
            b.queueLen = r.queueLen;
            balancer_->RecordQueueLength(id, r.queueLen);
        }
        if (r.hasGpu) {
            b.hasGpu = true;
            b.gpuUtil01 = r.gpuUtil01;
            b.vramUsedMb = r.vramUsedMb;
            b.vramTotalMb = r.vramTotalMb;
            balancer_->RecordGpuUtil(id, r.gpuUtil01, r.vramUsedMb, r.vramTotalMb);
        }
        if (r.hasModelLoaded) {
            b.aiReadyPresent = true;
            b.aiReady = r.modelLoaded;
            b.hasModelLoaded = true;
            b.modelLoaded = r.modelLoaded;
        }
        if (r.hasModelName) {
            b.hasModelName = true;
            b.modelName = r.modelName;
            if (b.modelLoaded && !b.modelName.empty()) {
                auto mit = modelAffinity_.find(b.modelName);
                if (mit == modelAffinity_.end() || mit->second.empty()) {
                    modelAffinity_[b.modelName] = id;
                }
            }
        }
        if (r.hasModelVersion) {
            b.hasModelVersion = true;
            b.modelVersion = r.modelVersion;
            if (b.modelLoaded && !b.modelVersion.empty()) {
                const std::string mv = b.modelName.empty() ? (std::string("@") + b.modelVersion) : (b.modelName + "@" + b.modelVersion);
                auto mit = modelVersionAffinity_.find(mv);
                if (mit == modelVersionAffinity_.end() || mit->second.empty()) {
                    modelVersionAffinity_[mv] = id;
                }
            }
        }

        const int oldW = b.weight;
        RecomputeWeightLocked(b);
        if (b.weight != oldW && IsEligibleLocked(b)) {
            balancer_->AddNode(id, b.weight);
        }
    }

    const bool newEligible = IsEligibleLocked(b);
    if (oldEligible != newEligible) {
        if (newEligible) balancer_->AddNode(id, b.weight);
        else balancer_->RemoveNode(id);
    }

    if (b.aiReadyPresent && (!oldAiReadyPresent || oldAiReady != b.aiReady)) {
        LOG_INFO << "Backend " << id << " ai_ready=" << (b.aiReady ? "true" : "false")
                 << " model_loaded=" << (b.modelLoaded ? "true" : "false");
    }
}

void BackendManager::OnCheckResult(const proxy::network::InetAddress&, bool) {
    // Handled in lambda above
}

} // namespace balancer
} // namespace proxy
