# 产品链接

[微雪电子 ESP32-S3-RLCD-4.2](https://www.waveshare.net/shop/ESP32-S3-RLCD-4.2.htm)

# 编译配置命令

**克隆工程**

```bash
git clone https://github.com/78/xiaozhi-esp32.git
```

**进入工程**

```bash
cd xiaozhi-esp32
```

**配置编译目标为 ESP32S3**

```bash
idf.py set-target esp32s3
```

**打开 menuconfig**

```bash
idf.py menuconfig
```

**选择板子**

```bash
Xiaozhi Assistant -> Board Type -> Waveshare ESP32-S3-RLCD-4.2
```

**编译**

```ba
idf.py build
```

**下载并打开串口终端**

```bash
idf.py build flash monitor
```

# 桌面助手功能

该板型默认提供天气/日历首页和音乐歌词页：

- Wi-Fi 状态直接读取当前连接和 RSSI；电量读取板载 ADC。
- 蓝牙状态仅在启用 ESP-BluFi 配网时可用，显示的是当前 BluFi BLE 会话状态。默认热点配网构建会显示“蓝牙 关闭”。
- 天气和备忘写入 NVS，重启后仍保留。
- 到点提醒为一次性提醒，会停止正在播放的音乐、切回首页并播放提示音。
- 音乐支持 HTTP/HTTPS MP3 直链、LRC 文本或 LRC URL，并同步显示换行歌词和播放进度；网关播放列表支持语音搜索、播放、暂停、恢复和切歌。
- 屏幕右侧 USER 键（GPIO18）在空闲时单击切换首页/音乐页；播放时单击暂停/继续，双击下一首，长按上一首。BOOT 键继续用于语音对话。
- 电量按板载 ADC 电压映射为百分比。充电状态没有接入 ESP32 GPIO，屏幕无法准确读取；请以板上的 CHG 指示灯判断是否正在充电。

## TF 卡儿童音乐与缓存

插入 FAT32 TF 卡后，将 MP3 放在卡根目录的 `儿童音乐` 文件夹内（可再分子文件夹）。设备会按文件路径排序，最多扫描 256 首、目录深度最多 4 层。固件不会格式化 TF 卡；没有卡或挂载失败时，在线音乐和语音助手仍可正常使用。

GPIO18 按键可直接播放本地列表，语音也可以调用 `self.music.local.play(index)`、`self.music.local.search(query)`。在线 MP3 首次播放时会写入卡内 `.xiaozhi/cache`；后续相同歌曲优先使用缓存。缓存上限为 512 MiB，并预留至少 8 MiB 空间；空间不足时按最近使用时间清理旧缓存，不会删除 `儿童音乐` 内的文件。未完整下载的临时文件会在下次挂载时清理。

天气数据不在固件内伪造，也不在设备上保存第三方天气密钥。AI 应先通过外部天气服务取得实时结果，再调用：

```text
self.weather.update(city, condition, temperature_c, humidity_percent, updated_at)
```

## 多源音乐网关

设备已兼容 [go-music-api](https://github.com/guohuiyuan/go-music-api) 的 REST API。该网关会并发搜索网易云、QQ 音乐、酷狗、酷我、咪咕等来源，并在原歌曲不可播放时按歌名、歌手和时长自动寻找可播放的替代音源。ESP32 不保存各平台账号或 Cookie，只保存网关基础地址。

在与设备互通的电脑或服务器上启动网关：

```bash
cd docker/music-gateway
docker compose up -d
```

然后把 `192.168.1.20` 替换为运行 Docker 的局域网地址：

```text
self.music.gateway.configure(url="http://192.168.1.20:8080")
self.music.gateway.status()
self.music.search(query="稻香 周杰伦", source="all")
self.music.play(index=1)
self.music.stop()
```

`source` 可使用 `all`、`netease`、`qq`、`kugou`、`kuwo` 或 `migu`。`all` 默认聚合多个平台；若只想使用网易云，传 `netease`。免登录模式只能播放各平台公开允许访问的内容，受版权、地区或会员限制的歌曲可能不可用；设备会请求网关自动换源，但不会绕过平台权限。网关本身没有访问鉴权，建议只开放在可信局域网或通过自有反向代理保护。

仍然保留直接播放外部 MP3/LRC URL 的接口：

```text
self.music.play_url(url, title, artist, lyric, lyric_url)
```

备忘和页面工具：

```text
self.memo.add(content, time)
self.memo.list()
self.memo.done(index)
self.memo.clear()
self.disp.switch(page)
```

`time` 可为空，或使用 `HH:MM`、`YYYY-MM-DD HH:MM`；最多保存 8 条。`page` 为 `weather` 或 `music`。
