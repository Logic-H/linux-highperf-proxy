# 性能测试方法与环境

本文档用于对应 `project.txt` 中“详细的性能测试方法和环境”要求。

## 1. 环境建议

- OS: Linux Kernel 5.4+（推荐更高）
- 编译器: `g++` 支持 C++17
- 文件描述符：压测前建议 `ulimit -n 200000`
- CPU governor：建议固定到 `performance`（可选）

## 2. 构建

```bash
mkdir -p build
cmake -S . -B build
cmake --build build -j
```

## 3. 基准测试工具

项目内置压测脚本：`scripts/benchmark.py`，具备自动启动/停止 server 的能力（带全局 timeout 防卡死）。

### 3.1 HTTP /stats 基准 (http_stats)

示例（对比 epoll/poll/select/uring，并输出 JSON）：

```bash
python3 scripts/benchmark.py --mode all --include-uring --bench http_stats \
  --host 127.0.0.1 --port 8080 --concurrency 200 --total 20000 \
  --spawn-server --server-bin ./build/proxy_server --server-config ./config/bench.conf \
  --global-timeout 60
```

输出示例：`bench_results_all.json`（包含每种 I/O 模型的 QPS、p50/p90/p99 延迟）。

补充样例（用于验证 `P99 < 10ms` 的轻负载场景）：

```bash
python3 scripts/benchmark.py --mode epoll --bench http_stats \
  --host 127.0.0.1 --port 8086 --concurrency 10 --total 20000 \
  --spawn-server --server-bin ./build/proxy_server --server-config ./config/bench_lat.conf \
  --per-mode-timeout 120 --global-timeout 200 --output bench_latency_epoll_20k_c10.json
```

### 3.2 连接保持/资源消耗基准 (connect_hold)

示例（大量连接保持一段时间，采样 CPU/RSS/FD）：

```bash
python3 scripts/benchmark.py --mode epoll --bench connect_hold \
  --host 127.0.0.1 --port 8080 --concurrency 5000 --hold 8 \
  --spawn-server --server-bin ./build/proxy_server --server-config ./config/bench.conf \
  --global-timeout 60
```

输出示例：`bench_conn_hold.json`（包含 server 进程 CPU 单核占比、RSS、FD 数）。

## 4. 结果采集与报告

- I/O 模型对比：优先使用 `--mode all` 导出 JSON（便于写报告/画图）
- 资源使用：使用 `connect_hold` 采样 CPU/RSS/FD
- 关键指标建议写入报告：QPS、p50/p90/p99 latency、CPU%、RSS、FD、失败率

## 5. 性能优化技术报告（补充）

本节用于覆盖 `project.txt` 的「性能优化技术报告」核心内容，强调“实现了哪些优化、为什么有效、代价是什么、如何验证”。

### 5.1 网络与 I/O

- 事件驱动：Reactor + Poller（epoll/poll/select/io_uring），用非阻塞 I/O 降低线程阻塞与上下文切换成本。
- 多线程 Reactor：`EventLoopThreadPool` 提升多核利用率（One Loop Per Thread）。
- `SO_REUSEPORT`：支持多实例水平扩展，降低单点瓶颈。

### 5.2 连接与协议

- Keep-Alive：前端与后端均尽量保持连接，减少握手开销。
- 后端连接池：复用后端连接，降低新建连接抖动。
- WebSocket Upgrade：升级后使用 tunnel，全双工转发避免重复解析。
- HTTP/2(h2c)/gRPC：按帧/流处理，避免将大请求一次性拷贝到内存。

### 5.3 内存与拷贝

- SlabAllocator：小对象固定尺寸分配，降低碎片与 malloc 压力。
- BuddyAllocator：大块内存按 2^k 拆分/合并，并对 idle arena 做 LRU 回收，降低 RSS 长期膨胀。
- Buffer 复用：读写路径尽量使用 append/move，减少中间字符串拼接与复制。

### 5.4 可观测与安全对性能的影响控制

- 统计缓存：`/stats` 使用短 TTL cache，降低高频拉取时的格式化开销。
- 日志级别过滤：压测时将 `log_level=ERROR`，避免 I/O 输出成为瓶颈。
- 早拒绝：ACL/限流/拥塞控制在尽可能早的阶段拒绝请求，减少资源占用。

## 6. 竞品对比与场景表现（补充）

竞品性能对比（HAProxy）与不同场景（小包/4KB/1MB）下的表现见：`docs/competitor_benchmark.md`。
