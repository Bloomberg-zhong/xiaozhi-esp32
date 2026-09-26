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

# 省电

- 空闲 3 分钟（没有对话、没有播放音乐）后自动进入省电模式：停止唤醒词和麦克风，CPU 进入
  light sleep（`CONFIG_PM_ENABLE` + tickless idle），屏幕切换到 ST7305 低功耗刷新模式，
  画面保持显示。
- 省电期间语音唤醒不可用，按 BOOT 键唤醒；有对话、通知或音乐开始时也会自动退出省电。
- 插着 USB 长期供电、希望唤醒词一直可用时，可以对小智说“关闭自动休眠”
  （工具 `self.power.set_auto_sleep`，保存在 NVS `wifi.sleep_mode`）。
