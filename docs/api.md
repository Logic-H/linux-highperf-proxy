# API 接口文档（OpenAPI/Swagger）

本页为 API 参考与运维接口说明；OpenAPI 规范见 [OpenAPI](/openapi)。

## 1. 监控与可视化

### `GET /stats`

- 返回：JSON
- 用途：运行状态、连接数、吞吐、延迟、内存、后端健康与负载等。

### `GET /dashboard`

- 返回：HTML
- 用途：实时仪表盘（前端每 1s 轮询 `/stats`）。

### `GET /history?seconds=60`

- 返回：JSON
- 用途：历史采样点查询（需要启用 `[history]`）。

### `GET /history_ui`

- 返回：HTML
- 用途：历史图表页（从 `/history` 拉取 points 渲染）。

### `GET /history/summary?seconds=300`

- 返回：JSON
- 用途：历史汇总/统计（需要启用 `[history]`）。

## 2. 配置管理（Web）

### `GET /config`

- 返回：HTML
- 用途：配置管理 UI（读取/编辑/保存 INI）。

### `GET /admin/config`

- 返回：JSON
- 用途：导出当前加载的配置快照与配置文件路径。

### `POST /admin/config`

- 入参：JSON
  - `updates`: `[{section,key,value}, ...]`
  - `deletes`: `[{section,key}, ...]`
  - `delete_sections`: `["sectionA", "sectionB", ...]`
  - `save`: `0/1`（是否写回文件）
- 返回：JSON `{ok, saved}`
- 备注：该接口默认受 ACL/限流保护（不做匿名开放）。

### `GET /admin/config?format=ini`

- 返回：`text/plain`（INI 文本）
- 用途：配置即代码（Config-as-code）：以 INI 形式导出当前配置。

### `POST /admin/config?format=ini&save=1`

- 入参：`text/plain`（INI 文本）
- 返回：JSON `{ok, saved}`
- 用途：配置即代码（Config-as-code）：用一份 INI 文本整体覆盖当前配置，并可选择写回文件。

## 3. 故障诊断与日志查看

### `GET /diagnostics`

- 返回：HTML
- 用途：诊断控制台（集成日志 tail 与综合诊断 JSON）。

### `GET /admin/diagnose`

- 返回：JSON
- 用途：综合诊断（stats + history_summary + config_file + audit_log_path + pid/时间戳）。

### `GET /admin/logs?type=audit&lines=N`

- 返回：`text/plain`
- 用途：查看审计日志 tail（需要启用 `[audit_log].path`）。

## 4. 动态服务发现 / 管理后端（Admin）

以下接口均为 HTTP/1.1，使用 JSON Body。

### `POST /admin/backend_register`

注册/更新后端：

```json
{"ip":"127.0.0.1","port":9001,"weight":1}
```

### `POST /admin/backend_remove`

移除后端：

```json
{"backend":"127.0.0.1:9001"}
```

或：

```json
{"ip":"127.0.0.1","port":9001}
```

### `POST /admin/backend_online`

上下线后端（不删除配置）：

```json
{"backend":"127.0.0.1:9001","online":1}
```

### `POST /admin/backend_weight`

更新后端基础权重（与 `service_discovery.auto_weight=1` 配合可实现“基于负载自动调权”）：

```json
{"backend":"127.0.0.1:9001","base_weight":10}
```

### `POST /admin/backend_metrics`

上报后端负载指标（队列/GPU/显存），用于智能算法与自动调权：

```json
{"backend":"127.0.0.1:9001","queue_len":12,"gpu_util":0.35,"vram_used_mb":2048,"vram_total_mb":16384}
```

## 5. 代理数据面（Data Plane）

- TCP 代理：L4 透明转发（由负载均衡选择后端）。
- UDP 代理：按客户端会话选择后端并转发（带空闲会话清理）。
- HTTP/1.1：L7 解析后按 path/model/version/affinity 路由。
- HTTP/2(h2c)/gRPC：支持 h2c + gRPC 帧处理与转发。
- WebSocket：支持 Upgrade，升级后全双工 tunnel 转发。

## 6. 插件扩展（Plugins）

- 默认插件路径前缀：`/plugin/*`（可通过 `[plugins].http_prefixes` 配置）
- 示例插件：`plugins/example_plugin.cpp`（可响应 `GET /plugin/hello`）
