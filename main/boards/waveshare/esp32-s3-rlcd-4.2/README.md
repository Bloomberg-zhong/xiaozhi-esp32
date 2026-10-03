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


# 桌面首页

待机默认显示桌面首页：时间、日期/星期、数字电量、城市天气、室内温湿度、备忘录和小智状态。
唤醒后显示对话，音乐开始后显示播放器；结束后返回桌面首页。
电量百分比（例如 `98%`）位于顶部电池图标左侧，所有页面共用；首页底部不再重复显示电量。

- KEY 只用于音乐控制。
- 语音：“回到桌面”“显示天气”。对应 `self.disp.switch`。
- 语音：“添加备忘录明天带水杯”“下午三点提醒我喝水”。也支持查询、完成、清空备忘。
  最多 8 条，首页显示前两条；定时提醒使用 `HH:MM` 或 `YYYY-MM-DD HH:MM`，到时提醒一次并移除。
- 城市天气：默认联网后通过 [IPIP MyIP](https://myip.ipip.net/) 定位公网 IP 所属城市，
  无有效城市时尝试 [IPWho](https://ipwhois.io/documentation)；定位结果在内存中缓存 6 小时。
  空闲时从 [Open-Meteo](https://open-meteo.com/en/docs) 获取真实天气，每 30 分钟刷新；失败 5 分钟后重试，
  对话/音乐期间延后查询，断网保留带时间戳的上次数据。可通过 `self.weather.get` 查询。
  兼容原来的 `self.weather.update` 外部天气写入接口。
  需要手动覆盖时，说“把桌面天气城市设置为上海”；说“恢复自动定位天气城市”可调用
  `self.weather.configure(city="auto")` 恢复 IP 定位。城市定位不替代室内传感器读数。
- 室内温湿度：板载 SHTC3，每 30 秒采集，校验 CRC，采完让传感器休眠；与城市天气独立显示。
- 时间由现有网络校时设置，板载 PCF85063 RTC 作为有效时间的离线备份；RTC 时间无效时显示等待校时。

沿用 `rlcd_dash` 和 `rlcd_memo` 的 NVS 数据；`rlcd_dash.wc_city` 保留手动天气城市，
新增 `rlcd_dash.wc_auto` 保存定位方式，缺省为自动，因此旧固件升级后也会启用 IP 定位。
电量来自电池 ADC，百分比显示在顶部电池图标旁边。连接电脑 USB 时通过
USB Serial/JTAG 的主机信号显示官方闪电电池图标；拔出后恢复对应电量图标。
充电芯片 STAT 只连接指示灯，没有接到 ESP32：闪电表示检测到电脑 USB 接入，
不能证明实际充电电流或“已充满”；普通充电器没有 USB 主机信号，仍显示电量图标。

# 语音回声与唤醒

[官方原理图](https://files.waveshare.com/wiki/ESP32-S3-RLCD-4.2/ESP32-S3-RLCD-4.2-schematic.pdf)
中的 ES7210 MIC1 是麦克风，MIC3 是 ES8311 DAC 经衰减电路返回的播放参考。
采集使用 TDM 槽 0/1，增益接口则按物理 MIC 编号选择 0/2，不能混用这两种编号。

麦克风与参考通道分别显式设为 30 dB。驱动在打开 ADC 时会重新配置各通道；
只设麦克风增益会使参考保持 0 dB，在该板上实测参考幅度不足，回答可能被识别为用户输入。
本板默认开启设备 AEC 和 `CONFIG_WAKE_WORD_DETECTION_IN_LISTENING`，对话方式沿用官方的实时模式：
回答时语音上行保持开启，直接开口就能打断，说“你好小智”或按 BOOT 键也能打断；回声消除沿用官方的
`AEC_MODE_FD_LOW_COST` + `AEC_NLP_LEVEL_VERYAGGR`，播放音乐时唤醒更可靠。回答的回声被识别成用户输入
的根源是参考通道增益不足，已由上面的 30 dB 参考增益解决。

`CONFIG_FORCE_AUTO_STOP_LISTENING`（轮流说话模式：回答时只检测唤醒词、播放队列清空后才恢复语音识别，
并改用 `AEC_MODE_SR_LOW_COST` 线性回声消除）仍然保留为可选项，但本板默认关闭：该模式下回答和音乐
期间只能靠唤醒词打断，而且回声抑制更弱，唤醒词在播放音乐时更容易被盖住。只有在调好参考增益后回声
仍被识别为用户输入时才建议打开。
修改参考增益后应检查正常回答和音乐的参考幅度、削波情况及真实唤醒，不能只看 AEC 已启用。

# 音乐播放

该板型默认启用 `CONFIG_USE_MUSIC_PLAYER`，可以用语音点歌（“播放周杰伦的稻香”“下一首”“暂停”）。
音乐来自你自己的 Navidrome/Subsonic 服务器或本地音乐文件夹，配置方法见
[docs/music-player.md](../../../../docs/music-player.md)。

播放器左侧显示音乐源提供的真实专辑封面，按原比例转为黑白抖动图。
音乐服务输出最长边 128 像素的 baseline JPEG；图片下载、解码与内存卡写入
使用单个后台任务，切歌后丢弃旧会话的图片。没有封面时显示“暂无封面”。
封面与歌曲一起存入 `music-cache`，断网仍可显示；自行放入内存卡的音乐可配
同名 `.jpg` 或目录中的 `cover.jpg`（baseline JPEG，最多 65536 像素、64 KiB）。

本板的音乐流、歌曲目录、天气、封面和缓存续传请求（连接编号 4–8）使用
`RlcdHttpClient`，通过 ESP-IDF 同步 HTTP 接口读写和释放连接，避免服务器断开或
切歌时旧接收回调访问已释放对象。其他网络请求继续使用现有网络实现。

TF 卡（SDMMC 1 线：CLK=GPIO38，CMD=GPIO21，D0=GPIO39，FAT32，不会自动格式化）挂载到 `/sdcard`：

- 放在卡上的音乐可以说“播放卡里的儿歌”来播放（`self.music.play_local`）。
- `white-noise/` 目录放白噪音，说“开始番茄钟”时在专注期间循环播放。

KEY 键（GPIO18）：音乐播放/暂停时，单击暂停/继续，双击下一首，三击切换播放模式
（顺序/列表循环/单曲循环/随机）；长按停止音乐，返回桌面首页。番茄钟通过语音控制。

空闲时长按 KEY，直接扫描内存卡并开始播放，无需语音或联网；
断网开机、等待 Wi-Fi 和配网页面也可长按启动。卡上任意音乐目录（例如 `儿童音乐/`）
均会递归扫描，跳过隐藏目录和 `white-noise/`，按路径排序，最多载入 100 首。
读取期间再次长按可取消；未插卡或没有歌曲时显示提示，不改动原播放列表。
插入或更换内存卡后需重启挂载。播放/暂停中长按仍是停止，再长按可以重新扫描播放。
离线播放中连上网络不会打断音乐；若尚未初始化语音助手，停止音乐后再完成联网初始化。

**音乐服务器域名**：`config.json` 的 `sdkconfig_append` 里已预留
`CONFIG_MUSIC_SERVER_URL=""` 和 `CONFIG_MUSIC_SERVER_API_KEY=""`，填上 `https://你的域名` 和密钥后重新编译；
也可以不重新烧录，在设备控制台调用 `self.music.configure_source(type="http", url=..., api_key=...)`。服务器
见 `scripts/music_server/README.md`。

播放音乐时，屏幕自动切换到独立播放器页，显示歌名、歌手、播放模式、已播放时间和歌词；
顶部保留网络和电量图标。未知总时长显示 `--:--`，没有歌词时显示“暂无歌词”。
KEY 单击暂停后自动返回桌面首页并保留播放进度，再单击继续播放并返回播放器页；
长按停止并返回首页。语音唤醒时切回对话页，对话结束恢复播放时再切回播放器页。
停止播放或顺序播放完列表后返回桌面首页。

# 省电

- 空闲 3 分钟（没有对话、没有播放音乐）后，屏幕切换到 ST7305 低功耗刷新模式，画面保持显示。
- 保留语音唤醒和麦克风，不进入需要按键才能唤醒的休眠；有对话、通知或音乐开始时自动恢复屏幕刷新。
- 可以对小智说“关闭自动休眠”来关闭屏幕自动省电
  （工具 `self.power.set_auto_sleep`，保存在 NVS `wifi.sleep_mode`）。
