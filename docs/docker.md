# Docker 部署

## Build

```bash
docker build -t highperf-proxy:latest .
```

## Run

默认配置会被复制到容器内 `/config/proxy.conf`，建议通过挂载覆盖配置：

```bash
docker run --rm -it \
  -p 8080:8080/tcp \
  -p 8080:8080/udp \
  -v "$PWD/config:/config:ro" \
  -v "$PWD/logs:/logs" \
  highperf-proxy:latest
```

## docker-compose

```bash
docker compose up --build
```

