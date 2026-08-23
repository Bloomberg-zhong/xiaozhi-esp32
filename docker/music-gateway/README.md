# XiaoZhi 多源音乐网关

该目录使用第三方开源项目 [go-music-api](https://github.com/guohuiyuan/go-music-api) 的公开 Docker 镜像，为 ESP32 提供统一的搜索、歌词、音频流代理和自动换源接口。

```bash
docker compose up -d
curl "http://127.0.0.1:8080/api/v1/music/search?q=稻香&type=song&sources=netease"
```

默认不配置任何平台账号。公开内容通常可以直接搜索和播放，但可用性仍取决于平台、版权、地区和会员策略。该服务采用 AGPL-3.0 许可证；这里没有复制或修改其代码，只引用上游镜像。若修改并对外提供该网关，请遵守上游许可证。

不要直接把 8080 端口暴露到公网。推荐仅在可信局域网使用，或放在带访问控制和 HTTPS 的反向代理后面。
