---
title: 配置说明文档
---

# 配置说明文档（Config）

本页为配置参考手册（INI），按模块说明用途、推荐值与常见坑。

配置文件为 INI 格式，默认示例：`config/proxy.conf`。

## 1. 全局配置 `[global]`

- `listen_port`：TCP 监听端口（HTTP/HTTPS/TCP 复用入口）
- `threads`：I/O 线程数（Reactor 线程池）
- `strategy`：负载均衡策略（如 `roundrobin`）
- `log_level`：`DEBUG/INFO/WARN/ERROR/FATAL`
- `io_model`：`epoll/poll/select/uring`
- `reuse_port`：`0/1` 是否启用 SO_REUSEPORT

## 2. TLS/ACME `[tls]`

- `enable`：`0/1` 启用 TLS 终止（同端口嗅探）
- `cert_path`：证书 PEM（full chain）
- `key_path`：私钥 PEM
- `acme_challenge_dir`：HTTP-01 challenge 文件目录（代理对外提供 `/.well-known/acme-challenge/<token>`）

## 3. 内存 `[memory]`

- `hugepage`：`0/1` slab chunk 使用 THP/hugepage 提示（best-effort）
- `slab_chunk_kb`：slab chunk 大小（KB）
- `buddy_enable`：`0/1` 启用 buddy（大块内存）
- `buddy_min_kb`：buddy 最小块（KB，2^k 建议）
- `buddy_arena_kb`：buddy arena 大小（KB，2^k 建议）
- `buddy_keep_arenas`：保持的 idle arena 数
- `buddy_max_arenas`：最大 arena 数（超过则 fallback malloc）

## 4. 连接管理 `[connection_limit]`

- `max_total`：全局最大连接数（0 不限制）
- `max_per_ip`：每 IP 最大连接数（0 不限制）
- `max_per_user`：每用户最大连接数（0 不限制）
- `user_header`：用户识别 header（默认 `X-Api-Token`）
- `user_max_entries`：用户表最大条目数
- `max_per_service`：每服务最大连接数（0 不限制）
- `service_max_entries`：服务表最大条目数
- `idle_timeout_sec`：空闲连接超时（0 不清理）
- `cleanup_interval_sec`：清理周期（秒）

## 5. 健康检查 `[health_check]` / AI 检查 `[ai_check]`

- `health_check.mode`：`tcp/http/script`
- `health_check.http_host/http_path`：HTTP 检查路径
- `health_check.script_cmd`：脚本检查命令（支持 `{ip}` `{port}` 占位符）
- `interval/timeout`：检查周期与超时

AI 检查：
- `ai_check.enable`：`0/1`
- `ai_check.http_host/http_path`：获取 GPU/VRAM/模型状态的路径

## 6. 批处理与预热

### `[batch]`（批量请求拆分/合并）

- `enable`：`0/1`
- `paths`：允许批处理的 path 前缀列表（逗号分隔）
- `max_batch`：最大 batch size
- `max_wait_ms`：等待聚合窗口
- `max_body_bytes`：单请求 body 上限（避免超大聚合）

### `[warmup]`（新后端预热）

- `enable`：`0/1`
- `model`：预热模型名
- `timeout`：超时（秒）
- `http_host/http_path`：预热请求路径

## 7. 动态服务发现 `[service_discovery]`

- `enable`：`0/1`
- `auto_weight`：`0/1` 根据负载动态调权（与 `backend_metrics` 上报配合）

## 8. UDP 代理 `[udp]`

- `enable`：`0/1`
- `listen_port`：UDP 监听端口
- `idle_timeout_sec`：UDP 会话空闲清理
- `cleanup_interval_sec`：清理周期

## 6. 限流与 DDoS

### `[rate_limit]`
- `qps/burst`：全局 TokenBucket
- `per_ip_*`：按 IP TokenBucket
- `per_path_*`：按 path TokenBucket

### `[ddos]`
- `accept_qps/accept_burst`：accept 级限速
- `per_ip_accept_*`：按 IP accept 限速

## 7. 拥塞控制 `[congestion]`

- `enable`：`0/1`
- `initial_window/min_window/max_window`：并发窗口（类似 cwnd）
- `additive_increase`：加性增步长
- `multiplicative_decrease`：乘性减系数（0~1）

说明：窗口满时代理将快速返回 `429` 并触发一次拥塞信号（loss）。

## 8. 调度与会话保持

### `[priority]`
- `enable`：`0/1`
- `mode`：`priority/fair/edf`
- `max_inflight`：最大并发转发数
- `high_threshold/low_delay_ms`：priority 模式参数
- `flow_*`：fair 模式 flow key 参数
- `deadline_*`：edf 模式 deadline 参数

### `[plugins]`（插件化扩展）

- `enable`：`0/1`
- `paths`：插件 `.so` 路径（逗号分隔）
- `http_prefixes`：仅将这些 path 前缀的请求分发给插件（逗号分隔，默认 `/plugin`）

## 8.1 L4 原始 TCP 隧道 `[l4]`

- `enable`：`0/1`
- `listen_port`：L4 监听端口（用于 raw TCP 转发，例如 `iperf3`）

### `[session_affinity]`
- `mode`：`none/ip/header/cookie`
- `header_name` / `cookie_name`

## 9. 流量镜像与缓存

### `[mirror]`（流量镜像）

- `enable`：`0/1`
- `udp_host/udp_port`：镜像接收端
- `sample_rate`：采样率（0~1）
- `max_bytes`：最大事件大小
- `max_body_bytes`：body 截断上限
- `include_req_body/include_resp_body`：是否包含请求/响应 body

### `[cache]`（Redis/Memcached best-effort）

- `enable`：`0/1`
- `backend`：`redis/memcached/off`
- `host/port`
- `ttl_sec`
- `timeout_ms`
- `max_value_bytes`

## 9.1 重写规则 `[rewrite:N]`

通过多个 `rewrite:N` section 定义请求/响应改写规则，常用于协议/字段兼容与灰度。

常用字段（详见配置示例注释）：
- `path_prefix`：path 前缀匹配
- `method`：HTTP 方法匹配
- `req_set_headers` / `req_del_headers`
- `req_body_replace`：形如 `A=>B;C=>D`
- `resp_set_headers` / `resp_del_headers`
- `resp_body_replace`

## 9. 审计日志与访问控制

### `[audit_log]`
- `path`：审计日志文件路径（空则禁用）

### `[access_control]`
- `ip_mode`：`off/allow/deny`
- `cidrs`：CIDR 列表（逗号分隔）
- `require_token/token_header/valid_tokens`
- `require_api_key/api_key_header/valid_api_keys`

## 10. 监控/告警 `[alerts]`

详见 `docs/monitoring.md`。

## 11. 示例（最小可用）

```ini
[global]
listen_port = 8080
threads = 4
strategy = roundrobin
log_level = INFO
io_model = epoll

[backend:1]
ip = 127.0.0.1
port = 9001
weight = 1
```

## 12. 后端配置 `[backend:N]`

后端通过多个 section 描述：

- `ip/port/weight`
- 可选指标（用于智能调度/自动调权）：
  - `queue_len`
  - `gpu_util`
  - `vram_used_mb/vram_total_mb`
