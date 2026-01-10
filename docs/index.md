# Voro

生产级高性能 TCP/UDP 代理与 L7 网关。

面向生产环境的高性能代理/负载均衡网关：支持 TCP/UDP 转发、HTTP/1.1（含 keep-alive/chunked）、WebSocket、HTTP/2(h2c)/gRPC，并提供 GPU/队列/响应时间等智能调度、限流与监控告警能力。

## 一键 Demo（推荐）

```bash
python3 scripts/demo.py --timeout 10
```

你应该能看到：
- Demo 后端启动成功（`19001/19002`）
- Proxy 启动成功（默认 `18080`）
- `/hello`、`/stats` 请求返回正常

## 快速入口

- [安装与运行](/install)
- [配置说明](/config)
- [架构设计](/architecture)
- [核心算法与调度](/algorithms)
- [监控与安全](/monitoring)
- [API 文档](/api)
- [OpenAPI 规范](/openapi)
- [性能测试方法](/performance)
- [资源使用分析](/resource_analysis)
- [水平扩展（SO\_REUSEPORT）](/scale_out)
- [演示环境](/demo)
- [典型使用场景](/use_cases)
- [故障排查](/troubleshooting)
- [仪表板展示](/dashboard_showcase)
- [竞品对比](/competitor_benchmark)
- [Docker 相关](/docker)
