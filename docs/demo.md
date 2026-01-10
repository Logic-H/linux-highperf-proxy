# 可运行的演示环境

面向首次上手：用一条命令跑通核心能力，并给出可验证的输出。

## 1. 一键演示脚本（推荐）

脚本会自动：

- 启动两个本地 HTTP 后端（`127.0.0.1:19001/19002`，提供 `/health` 与 `/hello`）
- 启动 Proxy（监听 `127.0.0.1:18080`）
- 通过 `/admin/backend_metrics` 注入 GPU/队列指标
- 请求 `/hello` 验证 GPU-aware 策略选择结果
- 请求 `/stats` 验证监控输出
- 自动停止所有进程/线程（带超时）

```bash
python3 scripts/demo.py --timeout 10
```

默认使用配置文件：`config/demo.conf`

## 2. 手工演示（可选）

1) 启动 Proxy：

```bash
./build/proxy_server -c ./config/demo.conf
```

2) 注入后端指标（示例）：

```bash
curl -s -X POST http://127.0.0.1:18080/admin/backend_metrics \\
  -H 'Content-Type: application/json' \\
  -d '{\"backend\":\"127.0.0.1:19002\",\"queue_len\":3,\"gpu_util\":0.10,\"vram_used_mb\":1024,\"vram_total_mb\":16384}'
```

3) 访问业务与监控：

```bash
curl -s http://127.0.0.1:18080/hello
curl -s http://127.0.0.1:18080/stats | head
```
