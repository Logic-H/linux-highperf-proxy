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
  - AI/GPU 指标（后端表格）：GPU 利用率、显存、队列长度、模型名/版本/是否已加载（需要后端上报或开启 AI 检查）
  - Raw JSON（用于快速复制排查）

### AI/GPU 指标数据来源

- 注入指标：`POST /admin/backend_metrics`（GPU/显存/队列）
- 注入模型状态：`POST /admin/backend_model`（模型/版本/loaded）
- 主动检查：开启 `[ai_check].enable=1`，代理会对后端拉取 `/ai/status`（后端需提供 JSON）

## 2. 历史分析 `/history`

启用 `[history].enable=1` 后可用：
- `GET /history?seconds=60`
- `GET /history/summary?seconds=300`

同时提供一个轻量历史图表页：
- `GET /history_ui`

## 3. 诊断页 `/diagnostics`

- 集成：
  - `GET /admin/logs?type=audit&lines=...`（审计日志 tail）
  - `GET /admin/diagnose`（综合诊断 JSON）

## 4. 建议演示脚本

启动代理后，可依次打开：
- `http://127.0.0.1:8080/dashboard`
- `http://127.0.0.1:8080/diagnostics`
- `http://127.0.0.1:8080/config`
