# 资源使用分析报告（方法与样例）

本文档用于对应 `project.txt` 中“资源使用分析报告”要求。

## 1. 指标来源

Proxy 的 `/stats` 会输出（JSON）：

- `process.rss_bytes`：进程 RSS（来自 `/proc/self/status`）
- `process.fd_count`：进程打开 FD 数（来自 `/proc/self/fd`）
- `process.cpu_time_sec`：进程累计 CPU time（来自 `/proc/self/stat`）
- `process.cpu_pct_single_core_avg`：以单核为基准的平均 CPU%（`cpu_time/uplink`）
- `bytes_in/bytes_out`：进出流量字节（读写路径累计）
- `udp_rx_drops`：UDP 接收队列溢出计数（`SO_RXQ_OVFL`，best-effort）

## 2. 采集方法

推荐使用 `scripts/benchmark.py` 的 `connect_hold` 模式自动采样并导出 JSON：

```bash
python3 scripts/benchmark.py --mode epoll --bench connect_hold \
  --host 127.0.0.1 --port 8080 --concurrency 5000 --hold 8 \
  --spawn-server --server-bin ./build/proxy_server --server-config ./config/bench.conf \
  --global-timeout 60
```

输出文件示例：`bench_conn_hold.json`

## 3. 分析建议

- FD：随着并发连接数上升，FD 应近似线性增长；超过 OS 限制会导致 accept/connect 失败
- RSS：观察是否随连接增长异常上升（泄漏/缓冲区过大）；结合 memory 统计定位
- CPU：在不同 I/O 模型、不同并发下对比 `cpu_pct_single_core_avg`（CPU 效率）
- UDP：`udp_rx_drops` 主要反映 socket 接收队列溢出（并非端到端丢包率）

## 4. 样例文件

- `bench_results_all.json`：I/O 模型 QPS/延迟对比
- `bench_conn_hold.json`：连接保持场景的 CPU/RSS/FD 采样

