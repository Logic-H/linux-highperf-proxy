import { defineConfig } from "vitepress";

export default defineConfig({
  title: "Proxy Server Docs",
  description: "High Performance TCP/UDP Proxy Server (Linux Assignment)",
  lang: "zh-CN",
  lastUpdated: true,
  cleanUrls: true,
  themeConfig: {
    nav: [
      { text: "指南", link: "/install" },
      { text: "配置", link: "/config" },
      { text: "架构", link: "/architecture" },
      { text: "API", link: "/api" }
    ],
    sidebar: [
      {
        text: "开始",
        items: [
          { text: "安装与运行", link: "/install" },
          { text: "配置说明", link: "/config" }
        ]
      },
      {
        text: "设计与实现",
        items: [
          { text: "架构设计", link: "/architecture" },
          { text: "核心算法与调度", link: "/algorithms" },
          { text: "水平扩展", link: "/scale_out" },
          { text: "监控与安全", link: "/monitoring" },
          { text: "故障排查", link: "/troubleshooting" }
        ]
      },
      {
        text: "接口与规范",
        items: [
          { text: "API 文档", link: "/api" },
          { text: "OpenAPI", link: "/openapi.yaml" }
        ]
      },
      {
        text: "测试与报告",
        items: [
          { text: "性能测试方法", link: "/performance" },
          { text: "资源使用分析", link: "/resource_analysis" },
          { text: "竞品对比", link: "/competitor_benchmark" },
          { text: "仪表板展示", link: "/dashboard_showcase" }
        ]
      },
      {
        text: "演示与场景",
        items: [
          { text: "演示环境", link: "/demo" },
          { text: "典型使用场景", link: "/use_cases" },
          { text: "Docker 相关", link: "/docker" }
        ]
      }
    ],
    search: { provider: "local" }
  }
});
