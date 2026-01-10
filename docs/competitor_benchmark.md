# 竞品性能对比与场景性能表现（本地基准）

本文档用于对应 `project.txt` 中：

- `o 与竞品的性能对比数据`
- `o 不同场景下的性能表现`

## 1. 测试对象

- 本项目：`build/proxy_server`
- 竞品：HAProxy（脚本自动下载源码并本地编译，二进制落地到 `third_party/haproxy/haproxy`）

## 2. 测试方法（可复现）

脚本：`scripts/bench_competitor.py`

- 在本机 `127.0.0.1` 启动一个 asyncio 后端（HTTP/1.1，短连接），提供：
  - `GET /ok`：2B 响应体
  - `GET /download?bytes=4096`：4KB 响应体
  - `GET /download?bytes=1048576`：1MB 响应体
- 分别启动两套前端代理（本项目 proxy vs HAProxy），转发到同一个后端
- 对每个场景分别压测两次（proxy 一轮、haproxy 一轮），统计：
  - QPS（`ok / elapsed_s`）
  - p50/p90/p99 延迟（ms）
  - `ok/failed`、总响应字节数

运行命令（带自动停止，带进度/ETA）：

```bash
timeout 1200s python3 scripts/bench_competitor.py \
  --global-timeout 600 \
  --concurrency 200 --total 20000 \
  --progress-interval 2 \
  --output bench_competitor_results_c200.json
```

## 3. 数据文件

本项目仓库内已生成并保留的原始数据（JSON）：

- `bench_competitor_results_c50.json`
- `bench_competitor_results_c200.json`
- `bench_competitor_results_c500.json`

## 4. 结果概览（QPS / p99）

说明：

- “更高 QPS”代表吞吐更好；“更低 p99”代表尾延迟更好。
- 该测试是 **localhost 回环**，更偏向比较“代理实现的 CPU/事件驱动开销”，不等价于 10GbE 实网吞吐。

### 4.1 并发 50（`--concurrency 50 --total 10000`）

| 场景 | proxy QPS | haproxy QPS | proxy p99(ms) | haproxy p99(ms) |
|---|---:|---:|---:|---:|
| small_2b `/ok` | 1769.39 | 1746.42 | 44.99 | 41.79 |
| download_4k | 1690.82 | 1590.90 | 50.68 | 51.19 |
| download_1m | 324.03 | 274.43 | 212.13 | 206.67 |

### 4.2 并发 200（`--concurrency 200 --total 20000`）

| 场景 | proxy QPS | haproxy QPS | proxy p99(ms) | haproxy p99(ms) |
|---|---:|---:|---:|---:|
| small_2b `/ok` | 1326.41 | 1199.78 | 250.00 | 1142.40 |
| download_4k | 1116.17 | 1395.77 | 249.95 | 184.22 |
| download_1m | 361.46 | 352.52 | 728.70 | 771.59 |

### 4.3 并发 500（`--concurrency 500 --total 5000`）

| 场景 | proxy QPS | haproxy QPS | proxy p99(ms) | haproxy p99(ms) |
|---|---:|---:|---:|---:|
| small_2b `/ok` | 1076.52 | 1018.49 | 2140.77 | 2148.86 |
| download_4k | 1002.13 | 1039.15 | 2325.84 | 2197.48 |
| download_1m | 319.29 | 356.28 | 3267.98 | 3020.19 |

## 5. 场景结论（不同场景性能表现）

- 小响应体（`/ok`）：在并发 50 时两者非常接近；并发 200 时本项目 QPS 更高，但 HAProxy 的 p99 出现显著波动（该现象可能与短连接/排队/调度有关，建议在报告中注明“本地回环 + 短连接 + Python 后端”的限制）。
- 中等响应体（4KB）：并发 200 时 HAProxy QPS 更高；并发 500 时两者接近。
- 大响应体（1MB）：两者整体 QPS 都明显下降（受带宽/拷贝/排队限制），在并发 200 时本项目略优；在并发 500 时 HAProxy 略优，且两者 p99 都进入秒级（明显排队）。

## 6. 注意事项（报告需注明）

- 该对比为单机 localhost 回环：不含真实网络、LRO/GRO、交换机等因素。
- 后端为 Python asyncio 的短连接 HTTP 服务器：吞吐/尾延迟会受 Python 事件循环影响。
- 两个代理均以“反向代理转发”工作，但配置并非生产级调优；如需更公平对比，可在同样的 keep-alive/连接复用条件下再跑一轮。

