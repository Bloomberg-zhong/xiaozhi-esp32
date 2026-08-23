# RLCD Dashboard Implementation Plan

**Goal:** Add real device status, externally sourced real weather, persistent reminders with alarm interruption, and MP3/LRC music playback to the Waveshare ESP32-S3-RLCD-4.2.

**Architecture:** Dashboard UI, persistence, and MCP tools remain owned by the RLCD board. Generic `Display` gains harmless music-page hooks. `Application` owns the focused HTTP MP3 playback lifecycle because it must coordinate the protocol, device state, wake-word interruption, and audio output. Weather credentials and music catalogs remain outside the device.

- [x] Add host-tested status formatting, weather validation, reminder validation/one-shot behavior, LRC parsing, and progress conversion.
- [x] Add a 400x300 weather/calendar dashboard and a separate music/lyrics page.
- [x] Connect live Wi-Fi RSSI, optional BluFi BLE session state, and ADC battery percentage.
- [x] Persist weather and up to eight reminders in bounded NVS JSON.
- [x] Add `self.weather.update`, `self.memo.*`, and `self.disp.switch` board MCP tools.
- [x] Poll reminders once per minute and schedule main-task alert interruption.
- [x] Add HTTP/HTTPS MP3 decoding, channel/rate conversion, bounded LRC download, synchronized lyrics, and wake/alert interruption.
- [x] Add `self.music.play_url` and `self.music.stop` board MCP tools.
- [x] Document the external weather and music-source contracts in the board README.
- [x] Run final host suite, formatting checks, and canonical board build.
- [ ] Flash and verify on physical hardware.
