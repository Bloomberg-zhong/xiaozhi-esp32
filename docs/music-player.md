# 音乐播放器 / Music Player

小智固件内置一个类似“小爱同学”的语音点歌播放器：对设备说“播放周杰伦的稻香”，
大模型调用设备端 MCP 工具，设备从**你自己的音乐库**搜索、流式下载并在本地解码播放；
播放中说唤醒词即可打断对话，对话结束后音乐自动继续。

The firmware contains a smart-speaker style music player. The assistant controls it
through the `self.music.*` MCP tools; songs are streamed from a self-hosted library
and decoded on the device.

## 启用 / Enabling

在 `idf.py menuconfig` → `Xiaozhi Assistant` → `Music Player` 中打开
`Enable music player`（`CONFIG_USE_MUSIC_PLAYER`），或在板型 `config.json` 的
`sdkconfig_append` 中加入 `"CONFIG_USE_MUSIC_PLAYER=y"`。该功能需要 PSRAM
（压缩音频缓冲放在 PSRAM，默认 256 KB，约 16 秒 128 kbps MP3）。

目前默认启用的板型：`waveshare/esp32-s3-rlcd-4.2`。

## 音乐源 / Music sources

设备不内置任何第三方平台的抓取逻辑，只连接你自己控制的服务：

| 类型 `type` | 服务 | 说明 |
|---|---|---|
| `subsonic` | [Navidrome](https://www.navidrome.org/)、Gonic、Airsonic 等 Subsonic/OpenSubsonic 服务器 | 推荐。服务器负责转码为 MP3（`max_bitrate_kbps`，默认 128），支持同步歌词（OpenSubsonic `getLyricsBySongId`）和随机播放。设备只保存加盐 token，不保存密码。 |
| `http` | 下文的简单 JSON API，例如 `scripts/music_server/music_server.py` | 适合直接共享一个本地音乐文件夹，或者自己写一个转接服务。 |

### 配置 / Configuration

运行时（推荐）：在小智控制台的设备工具中调用仅用户可见的工具
（the AI cannot see these tools）：

```text
self.music.configure_source(type="subsonic", url="http://192.168.1.10:4533",
                            username="me", password="******", max_bitrate_kbps=128)
self.music.configure_source(type="http", url="http://192.168.1.20:8090", api_key="")
self.music.configure_source(type="none")
self.music.get_source()
```

保存前会先测试连接（Subsonic `ping`，HTTP API 用一次空搜索）。配置保存在 NVS
命名空间 `music`（键：`type`、`url`、`user`、`salt`、`token`、`api_key`、`bitrate`、`mode`）。

编译时默认值：`menuconfig` → `Music Player` → `Default music source`。只在 NVS 中还没有
配置时使用；注意编译进固件的密码是明文。

## 语音控制 / Voice tools

| 工具 | 作用 |
|---|---|
| `self.music.play(query)` | 搜索并播放，结果作为播放队列（最多 20 首）；`query` 为空时随机播放。在后台任务中执行，不阻塞主循环。 |
| `self.music.control(action)` | `pause` / `resume` / `next` / `previous` / `stop` |
| `self.music.set_play_mode(mode)` | `sequence`（播完停止）/ `repeat_all` / `repeat_one` / `shuffle`，保存到 NVS |
| `self.music.get_status()` | 当前状态、歌曲、进度、播放模式、队列 |

音量沿用通用工具 `self.audio_speaker.set_volume`。

## 播放行为 / Behavior

- 新增设备状态 `playing`：只能从 `idle` 进入、回到 `idle`，由状态机统一管理。
- 在对话中点歌：先让模型把回复说完（等待播放队列排空），然后关闭音频通道并开始播放，
  和智能音箱一致。
- 播放中唤醒（唤醒词或按键）：音乐立即暂停，HTTP 连接和已缓冲的数据保留；对话结束回到
  `idle` 后自动续播。说“暂停/停止音乐”则不会续播。如果服务器在暂停期间断开连接，
  设备会用 `Range` 请求从断点续传（服务器不支持时跳过已下载的字节）。
- 与通知播放一样，只有 AFE 唤醒词（ESP32-S3/P4 等）能在播放中打断；其它芯片请用按键。
- 界面沿用小智原版 UI：状态栏显示歌名，聊天区显示“歌名 - 歌手”，有歌词时逐行显示。
- 网络中断时自动重连 3 次；一首歌失败会跳到下一首，连续 3 首失败后停止并提示错误。
- 支持的格式：MP3、AAC（ADTS）、M4A、FLAC、WAV。统一下混为单声道并重采样到编解码器
  输出采样率。建议让服务器转码为 MP3 以节省带宽和 CPU。

## HTTP JSON API

```text
GET {base}/search?q=<utf-8 query>&limit=<1..50>
Authorization: Bearer <api_key>      (only when an api_key is configured)
```

```json
{
  "tracks": [
    {
      "id": "album/track01.mp3",
      "title": "稻香",
      "artist": "周杰伦",
      "album": "魔杰座",
      "duration_ms": 223000,
      "url": "/files/album/track01.mp3",
      "lyric_url": "/lyrics/album/track01.mp3",
      "lyric": "[00:01.00]optional inline LRC"
    }
  ]
}
```

- `title` 和 `url` 必填，其余可选。`q` 为空表示随机/推荐。
- `url`、`lyric_url` 可以是绝对地址，也可以是相对于 `base` 的路径。设备只会把
  `Authorization` 头发给与 `base` 同源的地址。
- 音频地址应返回 2xx 和音频数据，建议支持 `Range`。设备最多跟随 3 次重定向。
- `lyric_url` 返回 LRC 纯文本（最多 64 KB）。

### 参考实现 / Reference server

```bash
python3 scripts/music_server/music_server.py --music-dir ~/Music --port 8090 [--api-key KEY]
```

只依赖 Python 标准库：递归扫描 `.mp3 .m4a .aac .flac .wav`，从 ID3v2 标签或
`歌手 - 歌名.mp3` 文件名读取信息，同名 `.lrc` 作为歌词，支持 `Range`。
请只在可信局域网内使用，或放在带 HTTPS 和访问控制的反向代理之后。

## 代码结构 / Code layout

- `main/music/music_player.*`：播放队列、会话（HTTP 读取任务 + 解码任务）、暂停/续播、歌词同步。
- `main/music/subsonic_source.*`、`http_api_source.*`：音乐源实现，接口见 `music_source.h`。
- `main/music/music_tools.*`：MCP 工具与设置加载。
- `main/music/lrc_parser.*`、`music_util.*`：无 ESP-IDF 依赖，主机测试见
  `scripts/tests/test_music_player_logic.py`。
- `Application` 负责状态切换（`TryStartMusic`、`SuspendMusicForChat`、`HandleMusicFinished`）。
