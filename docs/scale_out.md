# 无共享架构与水平扩展说明

本页说明如何用无共享方式水平扩展（多进程 + `SO_REUSEPORT`），以及生产部署注意事项。

## 1. 思路

当前 Proxy 采用事件驱动（Reactor）+ 连接状态本地化的实现方式：

- 单个进程内：`EventLoopThreadPool` 多 I/O 线程，连接对象/Buffer/统计在进程内维护
- 跨进程：通过 `SO_REUSEPORT` 允许多个 Proxy 进程绑定同一监听端口，从而实现水平扩展

这种方式的核心是“无共享”：多个进程之间不共享内存/锁，不存在全局单点瓶颈；扩容/缩容通过增加/减少进程数完成。

## 2. 使用方法（同机多进程）

1) 配置开启 `reuse_port`：

在配置文件 `[global]` 中设置：

```text
reuse_port = 1
```

2) 启动多个进程（示例 4 个）：

```bash
./build/proxy_server -c ./config/proxy.conf &
./build/proxy_server -c ./config/proxy.conf &
./build/proxy_server -c ./config/proxy.conf &
./build/proxy_server -c ./config/proxy.conf &
wait
```

内核会基于 `SO_REUSEPORT` 将新连接分发到不同进程，从而获得更高吞吐/更好的 CPU 利用率。

## 3. 说明

- `/stats` 为每个进程独立统计；生产部署可通过外部采集（如 Prometheus sidecar/日志）做聚合
- AI-aware 指标（GPU/队列）可通过 `/admin/backend_metrics` 对每个进程注入（或由外部服务统一广播）
