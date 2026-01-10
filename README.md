# Voro

Voro is a production-grade, high performance TCP/UDP proxy and L7 gateway for AI workloads.

专为智能计算（AI）场景优化的高性能 TCP/UDP 代理服务器，具备智能负载均衡、协议转换、深度监控和安全防护能力。

---

## 项目概述

在智能计算和大数据应用中，网络代理服务器扮演着关键角色：
- **负载均衡**：将海量 AI 推理请求分发到多台 GPU 服务器
- **协议转换**：统一不同后端服务的通信接口
- **安全隔离**：作为安全边界保护内部计算集群
- **流量监控**：实时分析 AI 服务的访问模式和性能瓶颈

本项目针对传统代理软件（如 Nginx、HAProxy）在智能计算场景下的局限，提供了深度 GPU 状态监控、AI 协议支持、智能调度等特性。

---

## 核心特性

### 高性能网络引擎
- **多模型 I/O 复用**：支持 `select` / `poll` / `epoll` / `io_uring`，运行时可切换
- **连接管理优化**：连接池复用、生命周期管理、僵尸连接清理、连接数限制
- **内存管理优化**：零拷贝技术、内存池（Slab/Buddy）、大页内存支持
- **性能基准**：
  - **并发连接数**：10,000+
  - **P99 延迟**：< 10ms（内网环境）
  - **吞吐量**：> 10Gbps（GCP 内网实测 19.21Gbps）
  - **CPU 效率**：10K 并发时 CPU 使用率 < 50%（实测 ~13.8%）

### 智能负载均衡
- **多种算法**：轮询、最少连接、一致性哈希、响应时间加权（EWMA）、队列长度、GPU 感知
- **健康检查**：TCP/HTTP 端口检查、自定义脚本、AI 服务检查（GPU 利用率、显存）
- **服务发现**：静态配置、动态注册、Consul/Etcd/K8s 集成
- **AI 特性**：
  - 模型亲和性路由
  - 批处理优化（智能合并小请求）
  - 预热管理（新节点模型预加载）
  - 模型版本路由（Model@Version）

### 协议处理
- **HTTP/1.1**：完整支持，包含 keep-alive、chunked 传输
- **HTTP/2 (h2c)**：多路复用、头部压缩（HPACK）
- **gRPC**：ProtoBuf 序列化、流式 RPC、HTTP/2 传输
- **WebSocket**：全双工通信、协议升级
- **协议转换**：HTTP <-> gRPC、H2 <-> H1.1、JSON <-> ProtoBuf、压缩算法转换
- **内容处理**：请求/响应修改、流量镜像、API 聚合、缓存集成（Redis/Memcached）

### 安全与监控
- **访问控制**：IP/CIDR、Token、API Key 白名单
- **流量防护**：全局限流、按 IP 限流、按接口限流、DDoS 基础防护
- **审计日志**：访问日志记录（IP、方法、路径、后端、耗时、拒绝原因）
- **实时告警**：阈值告警、异常检测（EWMA）、多渠道通知（邮件/Webhook/短信）、告警收敛
- **TLS 终止**：支持 TLS/SSL，集成 ACME HTTP-01 自动证书管理

### 监控与运维
- **实时仪表盘**：`/dashboard` 可视化关键指标
- **历史分析**：`/history` 历史数据查询与聚合
- **统计接口**：`/stats` JSON 格式输出，包含网络、进程、后端、业务指标
- **配置管理**：Web 界面 `/config`、管理 API `/admin/config`、配置即代码
- **故障诊断**：`/diagnostics` 诊断页、`/admin/logs` 日志查看
- **插件系统**：插件化架构，支持动态扩展功能

### 架构设计
- **无共享架构**：支持 SO_REUSEPORT 水平扩展
- **事件驱动**：多线程 Reactor 模型，One Loop Per Thread
- **高可用**：主从架构、故障自动转移、后端健康检查
- **容器化**：提供 Dockerfile 与 docker-compose.yml

---

## 快速开始

### 环境要求

- **操作系统**：Linux（Kernel 5.4+，推荐 Ubuntu 20.04+）
- **编译器**：GCC 9+ 或 Clang 10+（支持 C++17）
- **依赖库**：
  - `pthread`（多线程支持）
  - `openssl`（TLS 支持，可选）
  - `zlib`（压缩支持，可选）
  - `liburing`（io_uring 支持，可选，仅实验性）

### 构建

```bash
# 1. 克隆仓库
git clone https://github.com/Logic-H/linux-highperf-proxy.git
cd linux-highperf-proxy

# 2. 创建构建目录
mkdir build && cd build

# 3. 配置与编译
cmake ..
make -j$(nproc)
```

### 运行

```bash
# 使用默认配置启动
./proxy_server -c ../config/proxy.conf

# 查看帮助
./proxy_server -h
```

### 快速演示

```bash
# 运行自动化演示脚本
cd ..
python3 scripts/demo.py
```

---

## 配置示例

```ini
[server]
port = 8080
io_model = epoll
threads = 4

[backend]
# 后端服务器列表
backend1 = 127.0.0.1:9001
backend2 = 127.0.0.1:9002

[balancer]
# 负载均衡算法：round_robin / least_connections / consistent_hash / gpu_aware
algorithm = gpu_aware

[limit]
# 全局连接数限制
max_connections = 10000
# 每IP连接数限制
max_connections_per_ip = 100
# 全局限流（QPS）
global_qps = 10000

[monitor]
# 启用实时仪表盘
enable_dashboard = true
# 统计输出端口
stats_port = 8081
```

更多配置选项请参考 [docs/config.md](docs/config.md)。

---

## 性能基准

### 内网环境（GCP）
- **吞吐量**：19.21 Gbps（经 L4 隧道，iperf3 测试）
- **CPU 使用率**：4 核总占用 ~9.73%（单核均值 ~38.92%）
- **延迟**：P99 < 5.78ms

### 本地环境（Loopback）
- **10K 并发**：QPS 持续稳定，CPU 单核均值 ~13.81%
- **延迟**：P99 < 10ms

详细性能报告请参考：
- [docs/performance.md](docs/performance.md) - 性能测试方法与基准数据
- [docs/competitor_benchmark.md](docs/competitor_benchmark.md) - 与 HAProxy 的对比数据

---

## 文档

| 文档 | 说明 |
|------|------|
| [docs/install.md](docs/install.md) | 安装与部署指南 |
| [docs/config.md](docs/config.md) | 配置参数详解 |
| [docs/architecture.md](docs/architecture.md) | 架构设计与数据流 |
| [docs/api.md](docs/api.md) | API 文档与 OpenAPI 规范 |
| [docs/monitoring.md](docs/monitoring.md) | 监控指标与告警配置 |
| [docs/algorithms.md](docs/algorithms.md) | 负载均衡与调度算法 |
| [docs/use_cases.md](docs/use_cases.md) | 典型应用场景 |
| [docs/troubleshooting.md](docs/troubleshooting.md) | 故障排查指南 |
| [docs/docker.md](docs/docker.md) | Docker 容器化部署 |
| [docs/scale_out.md](docs/scale_out.md) | 水平扩展与高可用 |

---

## 测试

```bash
# 运行单元测试
cd build && make test

# 运行集成测试
cd ..
python3 scripts/run_tests.py

# 运行性能基准测试
python3 scripts/benchmark.py --mode epoll --concurrency 10000
```

---

## 贡献指南

欢迎提交 Issue 和 Pull Request！

1. Fork 本仓库
2. 创建特性分支 (`git checkout -b feature/AmazingFeature`)
3. 提交改动 (`git commit -m 'Add some AmazingFeature'`)
4. 推送到分支 (`git push origin feature/AmazingFeature`)
5. 开启 Pull Request

---

## 许可证

本项目采用 MIT 许可证 - 详见 [LICENSE](LICENSE) 文件。

---

## 致谢

- HAProxy：高性能负载均衡参考实现
- Linux 内核社区：epoll、io_uring 等网络基础设施
- Google：Protocol Buffers、gRPC 协议规范

---

## 联系方式

如有问题或建议，请通过 [Issues](https://github.com/Logic-H/linux-highperf-proxy/issues) 联系。
