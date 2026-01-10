---
title: 监控仪表板展示
---

# 监控仪表板展示（Dashboard Showcase）

本页展示内置 Dashboard/Diagnostics 的能力与使用方式。

## 1. 实时仪表盘 `/dashboard`

- 页面：`GET /dashboard`
- 数据源：每 1 秒轮询 `GET /stats`
- 展示内容（示例）：
  - Active Connections / Total Requests / Avg QPS
  - Backend Error Rate
  - P50/P90/P99 latency
  - Bytes In/Out
  - Process RSS / FD
  - Raw JSON（用于快速复制排查）

## 2. 历史分析 `/history`

启用 `[history].enable=1` 后可用：
- `GET /history?seconds=60`
- `GET /history/summary?seconds=300`

## 3. 诊断页 `/diagnostics`

- 集成：
  - `GET /admin/logs?type=audit&lines=...`（审计日志 tail）
  - `GET /admin/diagnose`（综合诊断 JSON）

## 4. 建议演示脚本

启动代理后，可依次打开：
- `http://127.0.0.1:8080/dashboard`
- `http://127.0.0.1:8080/diagnostics`
- `http://127.0.0.1:8080/config`
