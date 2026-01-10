---
title: 核心算法设计文档
---

# 核心算法设计文档（Algorithms）

本文档对应 `project.txt`「核心算法设计文档」。

## 1. 负载均衡算法

### 1.1 Round Robin / Weighted Round Robin

- 数据结构：后端列表 + 权重。
- 选择：按权重轮询选择健康且在线的后端。
- 失败处理：被动失败上报触发快速剔除/降权。

### 1.2 Least Connections

- 指标：后端活跃连接数（由 `BackendSession` 生命周期与连接池上报）。
- 选择：从健康后端中选择 `activeConnections` 最小者（权重可作为 tie-break）。

### 1.3 Consistent Hash（带虚拟节点）

- Hash：FNV-1a。
- 虚拟节点：每个后端映射到多个 ring points，提高均匀性与稳定性。
- 动态权重：权重变化通过虚拟节点数量或点位扩展实现，减少迁移。
- 使用场景：会话保持（IP/Header/Cookie）与路径组合生成 key。

### 1.4 ResponseTimeWeighted (EWMA)

- 指标：首包/首字节响应时间（EWMA 平滑）。
- 选择：根据 EWMA 进行加权选择（更快者权重更高）。
- 反馈闭环：每次后端首包到达时更新 EWMA。

### 1.5 GPU / 队列长度调度（AI-aware）

- 指标来源：健康检查 HTTP 返回字段，或配置/动态注册上报。
- 选择策略：
  - GPU 利用率低 + 队列短优先；
  - 支持与响应时间 EWMA 组合。

## 2. 连接调度算法（Request Scheduling）

系统支持三种调度模式，均以 `max_inflight` 限制代理到后端的并发转发量（线程内调度器）：

### 2.1 Strict Priority（优先级调度）

- 输入：`X-Priority`/`?priority=`（0-9）。
- 规则：达到 `high_threshold` 视为高优先级队列；否则进入低优先级。
- 调度：从高到低取队列，直到达到 `max_inflight`。
- 可选：`low_delay_ms` 延迟低优先级入队，降低优先级反转。

### 2.2 Fair Queuing（公平排队）

- 目标：避免单一流（flow）长期占用资源导致其他流饥饿。
- Flow key：`X-Flow`/`?flow=`，缺省使用 client IP。
- 调度：各 flow 轮询，每次从一个 flow 队列取 1 个任务（近似 DRR 的简化版）。

### 2.3 EDF（Earliest Deadline First）

- 输入：`X-Deadline-Ms`/`?deadline_ms=` 表示“相对当前时刻”的截止时间（ms）。
- 调度：最早截止时间的请求优先。
- 默认：`default_deadline_ms`。

## 3. 流量控制算法

### 3.1 Token Bucket（限流）

- 全局、按 IP、按 Path 三种粒度。
- 命中限流时返回 `429`。

### 3.2 Sliding Window + AIMD（拥塞控制）

- “窗口”：代理到后端的 **并发转发窗口**（类似 `cwnd`）。
- 成功：加性增（每 `cwnd` 次 ack 增 1）。
- 失败/拥塞：乘性减（`cwnd = floor(cwnd * beta)`，下限 `min_window`）。
- 目的：在后端不稳定或拥塞时自动降低并发，减少雪崩。

## 4. 内存分配算法

### 4.1 Slab（小对象）

- 多尺寸 slab（64B~64KB），用于 Buffer/小对象，降低 malloc 开销与碎片。

### 4.2 Buddy（大块内存）

- `minBlockBytes` 到 `arenaSizeBytes` 的 2^k 块拆分与合并。
- 适用：大于 64KB 的大块 Buffer/临时对象。
- LRU 回收：完全空闲的 arena 超过 `keepArenas` 时按 LRU 释放（降低 RSS）。

