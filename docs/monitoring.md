---
title: 监控和告警配置指南
---

# 监控和告警配置指南（Monitoring & Alerts）

本文档对应 `project.txt`「监控和告警配置指南」。

## 1. 监控数据

### 1.1 `/stats` 指标说明（摘要）

常用字段：
- `active_connections`：活跃连接数
- `total_requests`、`avg_qps`
- `latency_ms.{p50_ms,p90_ms,p99_ms,avg_ms}`
- `bytes_in/bytes_out`
- `process.{rss_bytes,fd_count,cpu_time_sec,cpu_pct_single_core_avg}`
- `memory.{slab_*,buddy_*,malloc_*}`
- `backends[]`：后端健康、权重、连接数、EWMA RT、错误率、队列/GPU/VRAM/模型等

### 1.2 历史采样

启用 `[history]` 后：
- `GET /history?seconds=...`
- `GET /history/summary?seconds=...`

## 2. 告警配置 `[alerts]`

### 2.1 开关与通用参数

- `enable=1`：启用告警
- `interval_sec`：采样/检测周期
- `cooldown_sec`：冷却时间（抑制告警风暴）
- `merge_window_sec`：短窗口内相同告警合并

### 2.2 阈值告警

支持基于如下阈值触发：
- `max_active_connections`
- `max_cpu_pct`（单核平均 CPU%）
- `max_rss_mb`
- `max_fd_count`
- `max_backend_error_rate`

### 2.3 异常检测（EWMA）

- `anomaly_enable=1`
- `anomaly_alpha`：EWMA alpha
- `anomaly_z`：Z-score 阈值
- `anomaly_min_samples`：最小样本数

## 3. 告警通道

配置字段：
- `webhook_url`：Webhook 通道
- `sms_webhook_url`：短信通道（以 Webhook 形式模拟）
- `email_*`：SMTP 邮件通道

建议：在生产中优先使用 Webhook 接入企业告警系统（Prometheus Alertmanager/自建平台）。

## 4. 推荐配置示例

```ini
[alerts]
enable = 1
interval_sec = 1.0
cooldown_sec = 30.0
merge_window_sec = 0.2
max_cpu_pct = 80
max_rss_mb = 2048
max_backend_error_rate = 0.01
webhook_url = http://127.0.0.1:9009/webhook
```

