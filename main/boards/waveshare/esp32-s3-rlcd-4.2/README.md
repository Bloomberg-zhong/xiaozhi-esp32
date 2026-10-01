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


# 音乐播放

该板型默认启用 `CONFIG_USE_MUSIC_PLAYER`，可以用语音点歌（“播放周杰伦的稻香”“下一首”“暂停”）。
音乐来自你自己的 Navidrome/Subsonic 服务器或本地音乐文件夹，配置方法见
[docs/music-player.md](../../../../docs/music-player.md)。

TF 卡（SDMMC 1 线：CLK=GPIO38，CMD=GPIO21，D0=GPIO39，FAT32，不会自动格式化）挂载到 `/sdcard`：

- 放在卡上的音乐可以说“播放卡里的儿歌”来播放（`self.music.play_local`）。
- `white-noise/` 目录放白噪音，说“开始番茄钟”时在专注期间循环播放。

KEY 键（GPIO18）：单击暂停/继续，双击下一首，三击切换播放模式（顺序/列表循环/单曲循环/随机），长按停止番茄钟和音乐。

**音乐服务器域名**：`config.json` 的 `sdkconfig_append` 里已预留
`CONFIG_MUSIC_SERVER_URL=""` 和 `CONFIG_MUSIC_SERVER_API_KEY=""`，填上 `https://你的域名` 和密钥后重新编译；
也可以不重新烧录，在设备控制台调用 `self.music.configure_source(type="http", url=..., api_key=...)`。服务器
见 `scripts/music_server/README.md`。

播放音乐时，屏幕自动切换到独立播放器页，显示歌名、歌手、播放模式、已播放时间和歌词；
顶部保留网络和电量图标。未知总时长显示 `--:--`，没有歌词时显示“暂无歌词”。
KEY 单击暂停后仍显示播放器页；语音唤醒时切回对话页，对话结束恢复播放时再切回播放器页。
停止播放或顺序播放完列表后返回待机界面。

# 省电

- 空闲 3 分钟（没有对话、没有播放音乐）后自动进入省电模式：停止唤醒词和麦克风，CPU 进入
  light sleep（`CONFIG_PM_ENABLE` + tickless idle），屏幕切换到 ST7305 低功耗刷新模式，
  画面保持显示。
- 省电期间语音唤醒不可用，按 BOOT 键唤醒；有对话、通知或音乐开始时也会自动退出省电。
- 插着 USB 长期供电、希望唤醒词一直可用时，可以对小智说“关闭自动休眠”
  （工具 `self.power.set_auto_sleep`，保存在 NVS `wifi.sleep_mode`）。
