---
title: 典型使用场景示例
---

# 典型使用场景示例（Use Cases）

面向生产使用：按场景给出可直接复用的请求/配置模式。

## 场景 1：AI 推理服务（模型/版本路由）

- 请求携带：
  - `X-Model: <name>`
  - `X-Model-Version: <version>`
- 代理根据模型/版本路由到对应后端，并可结合 GPU/队列指标进行调度。

示例：

```bash
curl -H 'X-Model: llama' -H 'X-Model-Version: v2' http://127.0.0.1:8080/infer
```

## 场景 2：高优先级请求优先处理（调度）

- 使用 `[priority]` 启用调度：
  - `mode=priority`：严格优先级
  - `mode=fair`：按 flow 公平
  - `mode=edf`：按 deadline 优先

示例（优先级）：

```bash
curl -H 'X-Priority: 9' http://127.0.0.1:8080/infer
```

## 场景 3：流式推理结果返回（SSE/流式）

客户端通过 `Accept: text/event-stream` 或 `X-Stream: 1` 提示代理走流式响应路径。

```bash
curl -N -H 'Accept: text/event-stream' http://127.0.0.1:8080/infer?stream=1
```

## 场景 4：安全边界（ACL + 限流 + 审计）

- `[access_control]`：IP/CIDR allow/deny、Token/API Key
- `[rate_limit]`：全局/按 IP/按 path 限流
- `[audit_log]`：启用审计日志写入

## 场景 5：运维与诊断

- 实时：`/dashboard`
- 历史：`/history`、`/history/summary`
- 配置：`/config`
- 诊断：`/diagnostics`、`/admin/diagnose`、`/admin/logs`
