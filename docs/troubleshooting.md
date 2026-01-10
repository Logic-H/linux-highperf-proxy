---
title: 故障诊断手册
---

# 故障诊断手册（Troubleshooting）

面向线上排障：从“症状”到“定位/缓解/根因”的快速路径。

## 1. 快速诊断入口

- `GET /diagnostics`：诊断控制台（HTML）
- `GET /admin/diagnose`：综合诊断 JSON（stats/history/config/audit）
- `GET /admin/logs?type=audit&lines=200`：审计日志 tail

## 2. 常见问题与处理

### 2.1 访问被拒绝（403/429）

- 403：检查 `[access_control]`（IP/CIDR、Token、API Key）
- 429：检查 `[rate_limit]`（全局/按 IP/按 path）或 `[congestion]`（拥塞控制窗口满）

### 2.2 后端不可用（502/503）

- 503：无可用后端（全部 down 或未注册）
- 502：后端连接失败/响应解析失败/中途断开

排查步骤：
1) `GET /stats` 查看 `backends[]` 的 `healthy/online/errorRate/failures`
2) 检查健康检查配置 `[health_check]`/`[ai_check]`
3) 若启用动态注册，检查 `/admin/backend_register` 调用与后端列表

### 2.3 TLS 相关问题

- 确认 `[tls].enable=1` 且 `cert_path/key_path` 指向有效 PEM
- 若使用 ACME：确认 `acme_challenge_dir` 可读，且外网可访问 `/.well-known/acme-challenge/<token>`

### 2.4 资源耗尽（FD/RSS/CPU）

建议：
- FD：`ulimit -n` 与 systemd `LimitNOFILE`
- CPU：降低日志级别、增加 `threads`、启用 `reuse_port` 多实例扩展
- RSS：检查 `memory.*`（buddy keep/max arenas），以及是否存在长连接/大响应

## 3. 诊断信息说明

`/admin/diagnose` 关键字段：
- `config_file`：当前加载配置文件路径
- `audit_log_path`：审计日志路径
- `stats`：同 `/stats`
- `history_summary`：历史汇总（如果启用 history）
