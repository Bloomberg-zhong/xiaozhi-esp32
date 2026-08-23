#include <cstdio>

#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include "application.h"
#include "button.h"
#include "codecs/box_audio_codec.h"
#include "config.h"
#include "custom_lcd_display.h"
#include "dashboard_store.h"
#include "lvgl.h"
#include "mcp_server.h"
#include "music_gateway_client.h"
#include "wifi_board.h"
#include "wifi_station.h"

#define TAG "waveshare_rlcd_4_2"

class CustomBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Button boot_button_;
    Button user_button_;
    CustomLcdDisplay* display_ = nullptr;
    adc_oneshot_unit_handle_t adc1_handle;
    adc_cali_handle_t cali_handle;
    bool vbat_status = 0;

    void InitializeI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {};
        i2c_bus_cfg.i2c_port = ESP32_I2C_HOST;
        i2c_bus_cfg.sda_io_num = AUDIO_CODEC_I2C_SDA_PIN;
        i2c_bus_cfg.scl_io_num = AUDIO_CODEC_I2C_SCL_PIN;
        i2c_bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
        i2c_bus_cfg.glitch_ignore_cnt = 7;
        i2c_bus_cfg.intr_priority = 0;
        i2c_bus_cfg.trans_queue_depth = 0;
        i2c_bus_cfg.flags.enable_internal_pullup = 1;
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

#if CONFIG_USE_DEVICE_AEC
        boot_button_.OnDoubleClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                app.SetAecMode(app.GetAecMode() == kAecOff ? kAecOnDeviceSide : kAecOff);
            }
        });
#endif

        user_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() != kDeviceStateIdle || display_ == nullptr) {
                return;
            }
            app.Schedule([display = display_]() { display->ToggleHomeMusicPage(); });
        });
    }

    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.disp.network", "重新配网", PropertyList(),
                           [this](const PropertyList&) -> ReturnValue {
                               EnterWifiConfigMode();
                               return true;
                           });

        mcp_server.AddTool(
            "self.weather.update",
            "把外部天气服务查询到的真实当前天气写入设备。调用前必须先查询真实天气，禁止猜测。",
            PropertyList({Property("city", kPropertyTypeString),
                          Property("condition", kPropertyTypeString),
                          Property("temperature_c", kPropertyTypeInteger, -100, 100),
                          Property("humidity_percent", kPropertyTypeInteger, -1, -1, 100),
                          Property("updated_at", kPropertyTypeString, std::string(""))}),
            [](const PropertyList& properties) -> ReturnValue {
                rlcd_dashboard::WeatherData weather;
                weather.city = properties["city"].value<std::string>();
                weather.condition = properties["condition"].value<std::string>();
                weather.temperature_c = properties["temperature_c"].value<int>();
                weather.humidity_percent = properties["humidity_percent"].value<int>();
                weather.updated_at = properties["updated_at"].value<std::string>();
                if (!rlcd_dashboard::DashboardStore::Instance().UpdateWeather(std::move(weather))) {
                    return std::string("天气数据无效，未更新");
                }
                return std::string("真实天气已写入设备并持久化");
            });

        mcp_server.AddTool(
            "self.memo.add",
            "添加持久化备忘或闹钟。time 为空表示仅展示；定时提醒使用 HH:MM 或 YYYY-MM-DD HH:MM。",
            PropertyList({Property("content", kPropertyTypeString),
                          Property("time", kPropertyTypeString, std::string(""))}),
            [](const PropertyList& properties) -> ReturnValue {
                auto item = rlcd_dashboard::DashboardStore::Instance().AddReminder(
                    properties["time"].value<std::string>(),
                    properties["content"].value<std::string>());
                if (!item.has_value()) {
                    return std::string("添加失败：时间格式无效、内容为空或备忘已满（最多 8 条）");
                }
                return std::string("已添加备忘，编号 ") + item->id;
            });

        mcp_server.AddTool("self.memo.list", "列出设备中的全部备忘和闹钟。", PropertyList(),
                           [](const PropertyList&) -> ReturnValue {
                               const auto items =
                                   rlcd_dashboard::DashboardStore::Instance().GetReminders();
                               if (items.empty()) {
                                   return std::string("当前没有备忘");
                               }
                               std::string result = "当前备忘：\n";
                               for (size_t index = 0; index < items.size(); ++index) {
                                   result += std::to_string(index + 1) + ". ";
                                   if (!items[index].time.empty()) {
                                       result += "[" + items[index].time + "] ";
                                   }
                                   result += items[index].content + "（" + items[index].id + "）\n";
                               }
                               return result;
                           });

        mcp_server.AddTool(
            "self.memo.done", "完成并删除一条备忘，index 为 self.memo.list 中从 1 开始的序号。",
            PropertyList({Property("index", kPropertyTypeInteger, 1, 8)}),
            [](const PropertyList& properties) -> ReturnValue {
                const auto items = rlcd_dashboard::DashboardStore::Instance().GetReminders();
                const int index = properties["index"].value<int>();
                if (index < 1 || static_cast<size_t>(index) > items.size()) {
                    return std::string("备忘序号无效");
                }
                const auto& item = items[static_cast<size_t>(index - 1)];
                if (!rlcd_dashboard::DashboardStore::Instance().RemoveReminder(item.id)) {
                    return std::string("删除备忘失败");
                }
                return std::string("已完成：") + item.content;
            });

        mcp_server.AddTool("self.memo.clear", "清空设备中的全部备忘和闹钟。", PropertyList(),
                           [](const PropertyList&) -> ReturnValue {
                               rlcd_dashboard::DashboardStore::Instance().ClearReminders();
                               return std::string("全部备忘已清空");
                           });

        mcp_server.AddTool("self.disp.switch", "切换 RLCD 页面，page 可为 weather 或 music。",
                           PropertyList({Property("page", kPropertyTypeString)}),
                           [this](const PropertyList& properties) -> ReturnValue {
                               const std::string page = properties["page"].value<std::string>();
                               if (page != "weather" && page != "music") {
                                   return std::string("页面无效，请使用 weather 或 music");
                               }
                               Application::GetInstance().Schedule([display = display_, page]() {
                                   if (page == "music") {
                                       display->SwitchToMusicPage();
                                   } else {
                                       display->SwitchToWeatherPage();
                                   }
                               });
                               return std::string("页面已切换到 ") + page;
                           });

        mcp_server.AddTool(
            "self.music.gateway.configure",
            "配置并持久化 go-music-api 音乐网关地址。仅在用户明确提供或确认地址时调用。",
            PropertyList({Property("url", kPropertyTypeString)}),
            [](const PropertyList& properties) -> ReturnValue {
                std::string error;
                if (!rlcd_dashboard::MusicGatewayClient::Instance().Configure(
                        properties["url"].value<std::string>(), error)) {
                    return error;
                }
                return std::string("音乐网关已配置并保存");
            });

        mcp_server.AddTool(
            "self.music.gateway.status", "查看设备当前配置的音乐网关地址。", PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                const std::string url = rlcd_dashboard::MusicGatewayClient::Instance().GetBaseUrl();
                return url.empty() ? ReturnValue(std::string("尚未配置音乐网关"))
                                   : ReturnValue(std::string("当前音乐网关：") + url);
            });

        mcp_server.AddWorkerTool(
            "self.music.search",
            "从音乐网关搜索歌曲。source 可为 all、netease、qq、kugou、kuwo、migu；all "
            "会并发聚合多源。",
            PropertyList({Property("query", kPropertyTypeString),
                          Property("source", kPropertyTypeString, std::string("all"))}),
            [](const PropertyList& properties) -> ReturnValue {
                std::string error;
                const auto songs = rlcd_dashboard::MusicGatewayClient::Instance().Search(
                    properties["query"].value<std::string>(),
                    properties["source"].value<std::string>(), error);
                if (songs.empty()) {
                    return error;
                }
                if (!McpServer::GetInstance().RunIfCurrentWorkerCall([&songs]() {
                        rlcd_dashboard::MusicGatewayClient::Instance().SetSearchResults(songs);
                    })) {
                    return std::string("音乐搜索已取消");
                }
                std::string result = "搜索结果（使用 index 播放）：\n";
                for (size_t index = 0; index < songs.size(); ++index) {
                    const auto& song = songs[index];
                    result += std::to_string(index + 1) + ". " + song.name;
                    if (!song.artist.empty()) {
                        result += " - " + song.artist;
                    }
                    result += " [" + song.source + "]";
                    if (song.duration_seconds > 0) {
                        const int minutes = song.duration_seconds / 60;
                        const int seconds = song.duration_seconds % 60;
                        char duration[16];
                        std::snprintf(duration, sizeof(duration), " %d:%02d", minutes, seconds);
                        result += duration;
                    }
                    result += "\n";
                }
                return result;
            });

        mcp_server.AddWorkerTool(
            "self.music.play",
            "播放 self.music.search "
            "返回的歌曲序号。播放前会探测可用性，失效时自动匹配其他公开音源。",
            PropertyList({Property("index", kPropertyTypeInteger, 1, 12)}),
            [](const PropertyList& properties) -> ReturnValue {
                std::string error;
                auto playback = rlcd_dashboard::MusicGatewayClient::Instance().ResolvePlayback(
                    static_cast<size_t>(properties["index"].value<int>()), error);
                if (!playback.has_value()) {
                    return error;
                }
                bool started = false;
                if (!McpServer::GetInstance().RunIfCurrentWorkerCall([&playback, &started]() {
                        started = Application::GetInstance().PlayMusicFromUrl(
                            playback->audio_url, playback->song.name, playback->song.artist, "",
                            playback->lyric_url);
                    })) {
                    return std::string("音乐播放请求已取消");
                }
                if (!started) {
                    return std::string("音乐播放启动失败；如果已有歌曲在播放，请先停止当前歌曲");
                }
                std::string result = "已开始播放：" + playback->song.name + " - " +
                                     playback->song.artist + " [" + playback->song.source + "]";
                if (playback->used_fallback) {
                    result += "（原音源不可用，已自动换源）";
                }
                return result;
            });

        mcp_server.AddTool(
            "self.music.play_url",
            "播放 HTTP/HTTPS MP3 直链。数据源由 AI 或外部音乐检索服务提供；可同时传 LRC 文本或 "
            "lyric_url。",
            PropertyList({Property("url", kPropertyTypeString),
                          Property("title", kPropertyTypeString, std::string("未知歌曲")),
                          Property("artist", kPropertyTypeString, std::string("未知歌手")),
                          Property("lyric", kPropertyTypeString, std::string("")),
                          Property("lyric_url", kPropertyTypeString, std::string(""))}),
            [](const PropertyList& properties) -> ReturnValue {
                const bool started = Application::GetInstance().PlayMusicFromUrl(
                    properties["url"].value<std::string>(),
                    properties["title"].value<std::string>(),
                    properties["artist"].value<std::string>(),
                    properties["lyric"].value<std::string>(),
                    properties["lyric_url"].value<std::string>());
                return started ? ReturnValue(std::string("已开始播放音乐"))
                               : ReturnValue(std::string("音乐播放启动失败"));
            });

        mcp_server.AddTool("self.music.stop", "停止设备当前播放的音乐。", PropertyList(),
                           [](const PropertyList&) -> ReturnValue {
                               Application::GetInstance().StopMusicPlayback();
                               return std::string("音乐已停止");
                           });
    }

    void InitializeLcdDisplay() {
        spi_display_config_t spi_config = {};
        spi_config.mosi = RLCD_MOSI_PIN;
        spi_config.scl = RLCD_SCK_PIN;
        spi_config.dc = RLCD_DC_PIN;
        spi_config.cs = RLCD_CS_PIN;
        spi_config.rst = RLCD_RST_PIN;
        display_ = new CustomLcdDisplay(NULL, NULL, RLCD_WIDTH, RLCD_HEIGHT, DISPLAY_OFFSET_X,
                                        DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y,
                                        DISPLAY_SWAP_XY, spi_config);
    }

    uint16_t BatterygetVoltage(void) {
        static bool initialized = false;
        static adc_oneshot_unit_handle_t adc_handle;
        static adc_cali_handle_t cali_handle = NULL;
        if (!initialized) {
            adc_oneshot_unit_init_cfg_t init_config = {
                .unit_id = ADC_UNIT_1,
            };
            adc_oneshot_new_unit(&init_config, &adc_handle);

            adc_oneshot_chan_cfg_t ch_config = {
                .atten = ADC_ATTEN_DB_12,
                .bitwidth = ADC_BITWIDTH_12,
            };
            adc_oneshot_config_channel(adc_handle, ADC_CHANNEL_3, &ch_config);

            adc_cali_curve_fitting_config_t cali_config = {
                .unit_id = ADC_UNIT_1,
                .atten = ADC_ATTEN_DB_12,
                .bitwidth = ADC_BITWIDTH_12,
            };
            if (adc_cali_create_scheme_curve_fitting(&cali_config, &cali_handle) == ESP_OK) {
                initialized = true;
            }
        }

        if (initialized) {
            int raw_value = 0;
            int raw_voltage = 0;
            int voltage = 0;  // mV
            adc_oneshot_read(adc_handle, ADC_CHANNEL_3, &raw_value);
            adc_cali_raw_to_voltage(cali_handle, raw_value, &raw_voltage);
            voltage = raw_voltage * 3;
            // ESP_LOGI(TAG, "voltage: %dmV", voltage);
            return (uint16_t)voltage;
        }

        return 0;
    }

    uint8_t BatterygetPercent() {
        int voltage = 0;
        for (uint8_t i = 0; i < 10; i++) {
            voltage += BatterygetVoltage();
        }

        voltage /= 10;
        int percent = (-1 * voltage * voltage + 9016 * voltage - 19189000) / 10000;
        percent = (percent > 100) ? 100 : (percent < 0) ? 0 : percent;
        // ESP_LOGI(TAG, "voltage: %dmV, percentage: %d%%", voltage, percent);
        return (uint8_t)percent;
    }

public:
    CustomBoard() : boot_button_(BOOT_BUTTON_GPIO), user_button_(USER_BUTTON_GPIO) {
        InitializeI2c();
        InitializeButtons();
        InitializeTools();
        InitializeLcdDisplay();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec audio_codec(
            i2c_bus_, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE, AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR, AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override { return display_; }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        charging = false;
        discharging = !charging;
        level = (int)BatterygetPercent();

        return true;
    }
};

DECLARE_BOARD(CustomBoard);
