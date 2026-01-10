# 文档站部署（VitePress）

本文档站以 `docs/` 作为源目录，构建产物在 `docs/.vitepress/dist/`。

## 本地预览

```bash
npm install
npm run docs:dev
```

## 构建静态站

```bash
npm run docs:build
```

## Cloudflare Pages

在 Cloudflare Pages 新建项目并关联仓库后：

- Build command: `npm run docs:build`
- Build output directory: `docs/.vitepress/dist`

若你的站点部署在子路径（例如 `https://example.com/proxy/`），需要在 `docs/.vitepress/config.mts` 设置 `base: "/proxy/"`。

## 自己服务器（Nginx）

1) 构建：

```bash
npm run docs:build
```

2) 将 `docs/.vitepress/dist/` 目录内容拷贝到服务器（例如 `/var/www/proxy-docs/`）

3) Nginx 示例：

```nginx
server {
  listen 80;
  server_name your.domain;

  root /var/www/proxy-docs;
  index index.html;

  location / {
    try_files $uri $uri/ /index.html;
  }
}
```

