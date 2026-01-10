# Docker 部署

## Build

```bash
docker build -t voro:latest .
```

## Run

默认配置会被复制到容器内 `/config/proxy.conf`，建议通过挂载覆盖配置：

```bash
docker run --rm -it \
  -p 8080:8080/tcp \
  -p 8080:8080/udp \
  -v "$PWD/config:/config:ro" \
  -v "$PWD/logs:/logs" \
  voro:latest
```

## docker-compose

```bash
docker compose up --build
```
