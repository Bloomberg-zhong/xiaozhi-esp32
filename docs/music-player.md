# 音乐播放器 / Music Player

小智固件内置类似智能音箱的语音点歌播放器：说“播放周杰伦的稻香”，大模型调用设备端 MCP 工具，设备从
**你自己的音乐服务器**（或 TF 卡）搜索、流式下载并在本地解码播放；播放中说唤醒词会暂停去对话，对话结束后
自动继续。

The firmware has a smart-speaker style music player. The assistant drives it through the `self.music.*`
MCP tools; songs are streamed from your own music server (or the SD card) and decoded on the device.

## 启用 / Enabling

`CONFIG_USE_MUSIC_PLAYER`（依赖 PSRAM，压缩音频缓冲放在 PSRAM，默认 256 KB，约 16 秒 128 kbps MP3）。
板型的 `config.json` 里加 `"CONFIG_USE_MUSIC_PLAYER=y"`，或 `idf.py menuconfig` → `Xiaozhi Assistant` →
`Music Player`。RLCD-4.2 已默认开启。

## 音乐服务器（域名参数） / Music server

推荐方式：部署 `scripts/music_server`（见其 README），它把多个**免费**音乐源放在一个地址后面，设备只需
知道你的域名：

| 来源 `source` | 内容 | 需要 |
|---|---|---|
| `archive` | Internet Archive：网络厂牌、现场录音、公版音乐 | 无 |
| `radio` | 网络电台（Radio Browser，含大量中文电台），直播流 | 无 |
| `jamendo` | Creative Commons 音乐 | 免费 client_id |
| `subsonic` | 你自己的 Navidrome / Gonic | 你的服务器 |
| `local` | 服务器上的一个音乐文件夹 | 文件夹 |

内置曲库不包含网易云、QQ 音乐等商业平台；已有音乐网关可通过 `gateway_plugin` 接入，见服务器 README。
找不到歌曲仅表示所配置的曲库未命中，不应直接解释为版权受限。自己有权使用的音乐
源可以写成服务器插件接入（见服务器 README）。

**域名写在哪里：**

1. 编译时：板型 `config.json` 的 `sdkconfig_append` 中已预留
   `"CONFIG_MUSIC_SERVER_URL=\"\""` 和 `"CONFIG_MUSIC_SERVER_API_KEY=\"\""`，把空字符串换成
   `https://你的域名` 和密钥即可（也可在 menuconfig 的 Music Player 里填）。
2. 运行时（无需重新烧录）：在设备控制台调用仅用户可见的工具
   `self.music.configure_source(type="http", url="https://你的域名", api_key="密钥")`。保存前会先测试连接。

域名为空且没配置过时，点歌会提示“没有配置音乐源”；有 TF 卡时默认从卡里找。配置保存在 NVS 命名空间
`music`（键：`type url user salt token api_key bitrate mode favs`）。

直连 Navidrome 也可以（`type="subsonic"`，只保存加盐 token，不保存密码），但推荐经音乐服务器，这样密码不在
设备上，还能同时用其它来源。

## 语音工具 / Voice tools

为减少每次对话发给大模型的提示词长度，工具合并为 6 个：

| 工具 | 作用 |
|---|---|
| `self.music.play(query, source, mode)` | 搜索并播放，结果成为播放列表（最多 20 首）。`source` 为空=默认（有服务器用服务器，否则 TF 卡），`sdcard`=TF 卡，`radio`/`jamendo`/`archive`…=服务器的来源；`mode` 可选，一句话“单曲循环播放稻香”即可 |
| `self.music.control(action)` | `pause` / `resume` / `next` / `previous` / `stop` |
| `self.music.set_play_mode(mode)` | `sequence` 顺序（播完停止）/ `repeat_all` 列表循环 / `repeat_one` 单曲循环 / `shuffle` 随机，保存到 NVS，屏幕提示模式名 |
| `self.music.queue(action, index, query, source, count)` | 播放列表：`list` 查看、`play` 跳到第 N 首、`add` 追加、`add_next` 下一首播放、`remove`、`clear`（序号从 1 开始，最多 100 首） |
| `self.music.favorites(action, index)` | 收藏：`list`、`add`（当前歌曲）、`remove`、`play`（整个收藏夹，从第 N 首开始） |
| `self.music.get_status()` | 状态、当前歌曲、进度、模式、列表长度 |

收藏最多 30 首，存在 NVS（键 `favs`，文本格式，约 3.6 KB 上限，因为 NVS 分区总共只有 16 KB）。服务器歌曲
只保存 `来源:ID` 和标题歌手，播放时用“服务器地址 + `/stream/<ID>`”直接拼出地址，不需要联网解析；更换服务器后
旧收藏会失效。TF 卡歌曲保存文件路径。

音量沿用 `self.audio_speaker.set_volume`。

## 播放行为 / Behavior

- 设备状态 `playing`：只能在 `idle` 与 `playing` 之间切换，由状态机统一管理。
- **点歌后结束对话**：AI 回复说完（`tts stop`）后设备关闭音频通道并开始播放；如果 AI 没有语音回复、或服务器
  一直保持聆听，7 秒后会强制结束对话开始播放（回复很长时最多再等 6×7 秒）。
- 播放中唤醒（唤醒词或按键）：音乐暂停，保留解码器和已缓冲数据，同时释放音乐 HTTP 连接，让语音通信
  使用网络接收资源；对话结束回到 `idle` 后用 `Range` 从已缓冲位置续传。说“暂停/停止”则不会自动续播。
  **电台直播例外**：暂停等于停止，
  恢复时重新接入当前直播，不会重放过时的缓冲。
- 唤醒后的问候语结束仍保持聆听；只有本轮明确点歌、继续或切歌时，回复结束才关闭对话并交给播放器。
  已经恢复播放后，迟到的语音通道关闭事件不会覆盖音乐的网络性能设置。
- 只有 AFE 唤醒词（ESP32-S3/P4）能在播放中打断；其它芯片请用按键。
- 失败处理：网络中断自动重连 3 次（直播流只要还有数据就持续重连）；一首歌失败跳下一首，连续 3 首失败后停
  止并提示。
- 界面沿用小智原版 UI：状态栏显示歌名，聊天区显示“歌名 - 歌手”，有歌词时逐行显示。
- 格式：MP3、AAC（ADTS）、M4A、FLAC、WAV，统一下混为单声道并重采样到输出采样率。
- 首次在线播放先准备歌词再读取音频，歌词服务慢或超时只影响开播前的准备时间，不会在开播后阻塞音频下载。
  普通在线歌曲预缓冲 128 KiB，欠载后积累 64 KiB 再继续；本地歌曲和直播使用较小的 16 KiB 门限。
  门限按实际分配容量缩小，短歌曲、下载结束和失败不会等到无法达到的水位；暂停、唤醒和停止仍可打断等待。

## 本地音乐（TF 卡）

板子实现 `Board::GetLocalMusicPath()`（RLCD-4.2 为 `/sdcard`）后，可播放卡上的 `.mp3 .m4a .aac .flac .wav`：
递归最多 4 层、500 首，跳过隐藏文件；`歌手 - 歌名.mp3` 解析出歌手，所在目录作为专辑，同名 `.lrc` 为歌词。
FAT32，中文文件名需要 `CONFIG_FATFS_LFN_HEAP` 和 `CONFIG_FATFS_API_ENCODING_UTF_8`（RLCD-4.2 已开启）。
`white-noise` 目录留给番茄钟，不会出现在普通搜索里。

插卡后，在线歌曲会边播放边写入 `music-cache`；完整下载长度和内容校验通过才发布为可播放文件。
提前切歌时，单个后台任务继续补齐下载（最多 32 个待完成任务）；唤醒、对话和暂停期间释放连接，回到空闲或播放状态后续传。
当前歌曲还在联网读取时，后台补齐等待；播放卡内歌曲、前台下载结束或停止后再补齐，避免多个音频下载争用网络接收缓冲区。
有稳定歌曲 ID 的待完成任务保存在卡上，下次使用相同音乐源播放时恢复；临时 URL 不写入待完成元数据，重启后需重新点歌。
服务器未提供完整长度、SD 写入失败或空间不足时继续在线播放；中断、损坏和未完成的 `.part` 不会列入离线歌单。
已缓存歌曲再次播放直接读卡，KEY 本地扫描也能找到它，保留歌名、歌手、专辑、时长及已保存的封面/歌词。
受管理缓存歌曲的封面缺失或损坏时，封面任务可使用保存的原始地址补图，并写回同一首歌的封面文件；已有正常封面直接读卡。
临时网络错误最多延迟重试两次，切歌或语音会中断补图；手动放入的普通本地音乐只读取本地图片。
默认点名播放时先查内存卡上的匹配歌曲，找不到才搜索音乐服务器；显式指定在线来源仍查该来源。
同一音乐源、目录和歌曲 ID 使用同一缓存身份，收藏也保留带前缀 ID 的来源。
后台补齐让位给正在点播的同一首歌；重播未完成歌曲先读取已校验的卡内片段，再用 Range 下载剩余字节。
已完成的文件在创建写入任务时再次检查，避免并发完成后重新下载。下载过的同步歌词单独保存在有校验标记的 `.lrc`，离线播放立即加载。
缓存上限 256 MiB，至少保留 32 MiB 空间；按时间删除带缓存标记的旧条目，保护正在播放/下载的歌曲及用户自行放入的文件。
空间不足或待完成任务达到 32 个时，清理最旧且未使用的受管理下载片段，避免旧音乐源留下的下载长期占用额度。
FAT 上替换受管理文件时先保留旧副本，重命名失败则回滚；续传记录发布失败后，最新有效的 `.part.meta` 仍可恢复下载。
替换多个文件不保证断电时的事务原子性：在旧文件移到 `.bak`、新文件尚未全部发布时断电，原文件可能仅保留在备份中，音频与元数据、封面或歌词可能暂不可用。
固件保留已有 `.bak` 并拒绝覆盖，不自动恢复这种中途断电的多文件事务；需要检查卡上的原文件与备份后恢复。

## 番茄钟 / Pomodoro

`CONFIG_USE_POMODORO`：

| 工具 | 作用 |
|---|---|
| `self.pomodoro.start(focus_min=25, break_min=5, white_noise=true)` | 专注倒计时，结束后自动进入休息（`break_min=0` 不休息）；重复调用重新开始 |
| `self.pomodoro.control(action)` | `pause` / `resume` / `stop` / `status` |

- 剩余时间按截止时刻计算，不会漂移；暂停时冻结剩余时长。
- 空闲或播放时状态栏显示“专注 24 分钟”，每 5 秒刷新；对话中不覆盖状态栏。
- 阶段结束先唤醒省电模式，再提示音 + 屏幕提示；对话中只显示通知。
- 白噪音：`<根目录>/white-noise/` 随机一首开始，整个目录循环；走音乐播放器，所以唤醒后暂停、对话结束继续。

## RLCD-4.2 按键（KEY，GPIO18）

| 操作 | 功能 |
|---|---|
| 单击（音乐播放/暂停时） | 暂停/继续音乐 |
| 双击（音乐播放/暂停时） | 下一首 |
| 三击（音乐播放/暂停时） | 切换播放模式：顺序 → 列表循环 → 单曲循环 → 随机 |
| 长按（空闲、等待 Wi-Fi 或配网时） | 从内存卡扫描并开始播放；无需联网或语音助手 |
| 长按（音乐播放/暂停、正在读卡时） | 停止音乐并返回桌面首页；正在读卡时取消 |

KEY 只用于音乐控制；日历的显示、翻月和返回首页通过语音完成，番茄钟也通过语音控制。
启动音乐后自动切到播放器，暂停音乐后返回首页，再单击继续播放。
按键读卡在后台任务中执行，最多 100 首，排除隐藏目录和 `white-noise/`；
未插卡、空目录或读取失败时只显示提示，不请求音乐服务器。
断网开机也可启动，联网后保持当前音乐，停止时再继续尚未完成的语音助手初始化。

## 省电与性能（RLCD-4.2）

- 空闲 3 分钟（可用 `self.power.set_auto_sleep(enabled, minutes)` 调整 1–60 分钟）进入省电：停止唤醒词和
  麦克风，CPU light sleep，屏幕低功耗模式；按 BOOT 唤醒。对话、通知、音乐开始时自动退出。
- 音频输出空闲 15 秒后关闭 DAC 和功放（双工编解码器以前为了不让 RX 卡住一直不关）。
- 电池电量每 30 秒读一次 ADC，而不是每秒十次。
- 屏幕刷新：像素位置用公式计算（原来是 PSRAM 里两张查找表，每像素两次缓存未命中），SPI 传输改为 DMA 完成
  中断释放 LVGL，不再在持有显示锁时阻塞主循环；这是“AI 有时反应迟钝”的一个主要原因。

## 代码结构 / Code layout

- `main/music/music_player.*`：播放队列与会话（HTTP/文件读取任务 + 解码任务）、暂停/续播、歌词同步、队列管理。
- `main/music/http_api_source.*`（音乐服务器）、`subsonic_source.*`：音乐源，接口见 `music_source.h`。
- `main/music/music_tools.*`：MCP 工具；`favorites.*`、`local_music.*`、`lrc_parser.*`、`music_util.*`：
  无 ESP-IDF 依赖，主机测试见 `scripts/tests/test_music_player_logic.py`。
- `main/pomodoro/`：番茄钟。
- `Application`（`TryStartMusic`、`SuspendMusicForChat`、`ArmMusicHandoff`、`HandleMusicFinished`）负责状态切换。
- `scripts/music_server/`：音乐服务器，测试见 `scripts/tests/test_music_server.py`。
