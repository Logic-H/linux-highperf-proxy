---
title: 安装部署指南
---

# 安装部署指南（多种环境）

面向生产环境的安装与运行指南。

## 1. 开发环境（本机编译运行）

### 1.1 依赖

- CMake >= 3.10
- C++17 编译器（g++/clang++）
- OpenSSL、Zlib、liburing（可选）

### 1.2 构建

```bash
mkdir -p build
cmake -S . -B build
cmake --build build -j
```

### 1.3 运行

```bash
./build/proxy_server -c ./config/proxy.conf
```

建议压测前：

```bash
ulimit -n 200000
```

## 2. 生产环境（裸机/VM）

### 2.1 配置建议

- `threads`：建议与 CPU 核数接近（或略小）。
- `reuse_port=1`：需要多实例扩展时启用。
- `log_level=ERROR`：性能压测/高负载建议减少日志。

### 2.2 systemd（示例）

创建 `/etc/systemd/system/proxy_server.service`：

```ini
[Unit]
Description=High Performance Proxy Server
After=network.target

[Service]
Type=simple
WorkingDirectory=/opt/proxy
ExecStart=/opt/proxy/proxy_server -c /opt/proxy/proxy.conf
Restart=always
RestartSec=1
LimitNOFILE=200000

[Install]
WantedBy=multi-user.target
```

启动：

```bash
systemctl daemon-reload
systemctl enable --now proxy_server
```

## 3. TLS/证书（ACME HTTP-01）

项目提供 `scripts/acme.py` 用于通过 ACME HTTP-01 获取证书（挑战文件由代理提供服务）。

典型流程：

1) 在配置中启用 challenge dir（`[tls].acme_challenge_dir=/path/to/challenge`）并启动代理  
2) 运行 `scripts/acme.py` 生成/更新证书 PEM  
3) 在配置中启用 TLS（`[tls].enable=1`，填入 `cert_path/key_path`）

说明：TLS 监听同端口，代理通过首字节嗅探区分 HTTP 与 TLS ClientHello。
