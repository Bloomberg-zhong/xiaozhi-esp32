#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <driver/usb_serial_jtag.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include <esp_network.h>
#include "application.h"
#include "button.h"
#include "codecs/box_audio_codec.h"
#include "config.h"
#include "custom_lcd_display.h"
#include "dashboard_sensors.h"
#include "dashboard_tools.h"
#include "dashboard_weather.h"
#include "lvgl.h"
#include "mcp_server.h"
#include "power_save_timer.h"
#include "rlcd_http_client.h"
#include "settings.h"
#include "wifi_board.h"
#include "wifi_station.h"
#if CONFIG_USE_MUSIC_PLAYER
#include "music/local_music.h"
#include "music/music_tools.h"
#endif

#include <driver/sdmmc_host.h>
#include <esp_timer.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>

#define TAG "waveshare_rlcd_4_2"

// Idle time before reducing panel refresh. Audio and wake-word detection
// remain active so the desktop assistant can always be woken by voice.
#define POWER_SAVE_IDLE_SECONDS 180  // Default; overridden by the wifi/sleep_seconds setting

namespace {
class RlcdNetwork : public EspNetwork {
public:
    std::unique_ptr<Http> CreateHttp(int connect_id = -1) override {
        // Music stream/catalog, weather, artwork and SD continuation requests
        // use synchronous HTTP cleanup, avoiding the vendor RX callback race.
        if (connect_id >= 4 && connect_id <= 8)
            return std::make_unique<RlcdHttpClient>();
        return EspNetwork::CreateHttp(connect_id);
    }
};
}  // namespace

class CustomBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Button boot_button_;
    Button user_button_;
    CustomLcdDisplay* display_;
    std::unique_ptr<DashboardSensors> dashboard_sensors_;
    sdmmc_card_t* sd_card_ = nullptr;
    PowerSaveTimer* power_save_timer_ = nullptr;
    adc_oneshot_unit_handle_t adc1_handle;
    adc_cali_handle_t cali_handle;
    bool vbat_status = 0;
    int battery_percent_ = -1;
    int64_t battery_read_us_ = 0;
#if CONFIG_USE_MUSIC_PLAYER
    // Only the application task reads/writes these; workers carry a revision.
    bool local_music_scan_pending_ = false;
    bool local_music_scan_running_ = false;
    uint32_t local_music_scan_revision_ = 0;
#endif

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

    void InitializeSdCard() {
        esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
        mount_config.format_if_mount_failed = false;  // Never touch the user's card
        mount_config.max_files = 4;
        mount_config.allocation_unit_size = 16 * 1024;

        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
        slot_config.width = 1;
        slot_config.clk = SD_CARD_CLK_PIN;
        slot_config.cmd = SD_CARD_CMD_PIN;
        slot_config.d0 = SD_CARD_D0_PIN;
        slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

        esp_err_t ret = esp_vfs_fat_sdmmc_mount(SD_CARD_MOUNT_POINT, &host, &slot_config,
                                                &mount_config, &sd_card_);
        if (ret != ESP_OK) {
            sd_card_ = nullptr;
            ESP_LOGW(TAG, "No SD card mounted: %s", esp_err_to_name(ret));
            return;
        }
        ESP_LOGI(TAG, "SD card mounted at %s", SD_CARD_MOUNT_POINT);
    }

    // KEY controls music only: hold while idle starts SD music, click
    // pauses/resumes, double click skips, triple click changes the play mode,
    // and hold during playback stops music and returns home.
    void InitializeUserButton() {
        user_button_.OnClick([this]() {
            power_save_timer_->WakeUp();
            Application::GetInstance().Schedule([this]() {
#if CONFIG_USE_MUSIC_PLAYER
                auto& app = Application::GetInstance();
                auto& player = app.GetMusicPlayer();
                if (player.IsPlaying()) {
                    app.PauseMusic();
                } else if (player.IsPaused()) {
                    app.PlayMusic(false);
                }
#endif
            });
        });
        user_button_.OnDoubleClick([this]() {
            power_save_timer_->WakeUp();
            Application::GetInstance().Schedule([this]() {
#if CONFIG_USE_MUSIC_PLAYER
                auto& app = Application::GetInstance();
                if (app.GetMusicPlayer().IsPlaying() || app.GetMusicPlayer().IsPaused()) {
                    app.SkipMusic(true);
                }
#endif
            });
        });
        user_button_.OnMultipleClick(
            [this]() {
                power_save_timer_->WakeUp();
                Application::GetInstance().Schedule([this]() {
#if CONFIG_USE_MUSIC_PLAYER
                    auto& app = Application::GetInstance();
                    if (app.GetMusicPlayer().IsPlaying() || app.GetMusicPlayer().IsPaused()) {
                        CycleMusicPlayMode();
                    }
#endif
                });
            },
            3);
        user_button_.OnLongPress([this]() {
            power_save_timer_->WakeUp();
            Application::GetInstance().Schedule([this]() {
#if CONFIG_USE_MUSIC_PLAYER
                auto& app = Application::GetInstance();
                auto state = app.GetDeviceState();
                bool stop = local_music_scan_pending_ || app.GetMusicPlayer().IsPlaying() ||
                            app.GetMusicPlayer().IsPaused();
                if (!stop && (state == kDeviceStateIdle || state == kDeviceStateStarting ||
                              state == kDeviceStateWifiConfiguring)) {
                    StartLocalMusic();
                    return;
                }
                ++local_music_scan_revision_;
                local_music_scan_pending_ = false;
                app.StopMusic();
                display_->RequestPage(rlcd_dashboard::DashboardPage::kHome);
#endif
            });
        });
    }

#if CONFIG_USE_MUSIC_PLAYER
    void StartLocalMusic() {
        if (local_music_scan_running_) {
            display_->ShowNotification("正在结束读卡，请稍候", 2000);
            return;
        }
        auto& app = Application::GetInstance();
        const std::string root = app.GetMusicPlayer().GetLocalRoot();
        if (root.empty()) {
            display_->ShowNotification("未检测到内存卡", 3000);
            return;
        }
        struct ScanRequest {
            CustomBoard* board;
            std::string root;
            uint32_t revision;
        };
        auto request =
            std::make_unique<ScanRequest>(ScanRequest{this, root, ++local_music_scan_revision_});
        local_music_scan_pending_ = true;
        local_music_scan_running_ = true;
        display_->ShowNotification("正在读取内存卡音乐", 3000);
        BaseType_t result = xTaskCreate(
            [](void* arg) {
                std::unique_ptr<ScanRequest> request(static_cast<ScanRequest*>(arg));
                LocalMusicScanOptions options;
                options.max_tracks = 100;  // Same bound as the playback queue
                options.excluded_folders.push_back(kWhiteNoiseFolder);
                auto tracks = ScanLocalMusic(request->root, options);
                auto* board = request->board;
                const uint32_t revision = request->revision;
                Application::GetInstance().Schedule(
                    [board, revision, tracks = std::move(tracks)]() mutable {
                        board->local_music_scan_running_ = false;
                        if (revision != board->local_music_scan_revision_)
                            return;  // A second hold cancelled this scan
                        board->local_music_scan_pending_ = false;
                        auto& app = Application::GetInstance();
                        const auto state = app.GetDeviceState();
                        const bool can_start =
                            state == kDeviceStateIdle || state == kDeviceStateStarting ||
                            state == kDeviceStateWifiConfiguring || state == kDeviceStateActivating;
                        if (!can_start) {
                            // A conversation now owns the device; tell the user instead of
                            // silently dropping the request.
                            board->display_->ShowNotification("对话中，请结束后再长按播放", 3000);
                            return;
                        }
                        if (tracks.empty()) {
                            board->display_->ShowNotification("内存卡里没有音乐", 3000);
                            return;
                        }
                        // Do not replace offline music with the timeout's config sound.
                        esp_timer_stop(board->connect_timer_);
                        ESP_LOGI(TAG, "KEY local library: %u tracks", unsigned(tracks.size()));
                        app.GetMusicPlayer().SetQueue(std::move(tracks), 0, false, "sdcard-key");
                        app.PlayMusic(true);
                    });
                request.reset();
                vTaskDelete(nullptr);
            },
            "sd_music_scan", 8192, request.get(), 2, nullptr);
        if (result != pdPASS) {
            local_music_scan_pending_ = false;
            local_music_scan_running_ = false;
            display_->ShowNotification("读取音乐失败，请重试", 3000);
            return;
        }
        request.release();  // Worker owns it after successful task creation
    }
#endif

    void InitializePowerSaveTimer() {
        Settings settings("wifi", false);
        int idle_seconds = settings.GetInt("sleep_seconds", POWER_SAVE_IDLE_SECONDS);
        // Keep the user's always-available wake word; idle saving only lowers
        // the RLCD refresh mode, rather than disabling microphone capture.
        power_save_timer_ = new PowerSaveTimer(-1, idle_seconds, -1);
        power_save_timer_->OnEnterSleepMode([this]() { display_->SetPowerSaveMode(true); });
        power_save_timer_->OnExitSleepMode([this]() { display_->SetPowerSaveMode(false); });
        power_save_timer_->SetEnabled(true);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            power_save_timer_->WakeUp();
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

#if CONFIG_USE_DEVICE_AEC
        boot_button_.OnDoubleClick([this]() {
            power_save_timer_->WakeUp();
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                app.SetAecMode(app.GetAecMode() == kAecOff ? kAecOnDeviceSide : kAecOff);
            }
        });
#endif
    }

    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.disp.network", "重新配网", PropertyList(),
                           [this](const PropertyList&) -> ReturnValue {
                               EnterWifiConfigMode();
                               return true;
                           });

        mcp_server.AddTool(
            "self.power.set_auto_sleep",
            "Enable or disable automatic RLCD power saving. After the idle time the display "
            "enters low power refresh mode; voice wake remains available.\n"
            "Args:\n"
            "  `enabled`: Turn automatic power saving on or off.\n"
            "  `minutes`: Idle minutes before sleeping (1-60, default 3).",
            PropertyList({Property("enabled", kPropertyTypeBoolean),
                          Property("minutes", kPropertyTypeInteger, 3, 1, 60)}),
            [this](const PropertyList& properties) -> ReturnValue {
                bool enabled = properties["enabled"].value<bool>();
                int minutes = properties["minutes"].value<int>();
                {
                    Settings settings("wifi", true);
                    settings.SetBool("sleep_mode", enabled);
                    settings.SetInt("sleep_seconds", minutes * 60);
                }
                power_save_timer_->SetSecondsToSleep(minutes * 60);
                power_save_timer_->SetEnabled(enabled);
                return true;
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
    CustomBoard()
        : boot_button_(BOOT_BUTTON_GPIO, false, 0, 0, true),
          user_button_(USER_BUTTON_GPIO, false, 0, 0, true) {
        InitializeI2c();
        InitializeButtons();
        InitializeUserButton();
        InitializeSdCard();
        InitializeTools();
        InitializeLcdDisplay();
        AddDashboardTools(display_);
        dashboard_sensors_ = std::make_unique<DashboardSensors>(i2c_bus_);
        if (!dashboard_sensors_->Start()) {
            ESP_LOGW(TAG, "Desktop sensor task unavailable");
        }
        InitializePowerSaveTimer();
        if (!DashboardWeather::Instance().Start()) {
            ESP_LOGW(TAG, "Desktop weather worker unavailable");
        }
    }

    virtual AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec audio_codec(
            i2c_bus_, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE, AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR, AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE,
            30.0f,   // Physical MIC1 microphone gain.
            2,       // Physical MIC3: DAC loopback, captured in TDM slot 1.
            30.0f);  // Compensate the board reference attenuator after ADC open.
        return &audio_codec;
    }

    virtual Display* GetDisplay() override { return display_; }

    NetworkInterface* GetNetwork() override {
        static RlcdNetwork network;
        return &network;
    }

    virtual const char* GetLocalMusicPath() override {
        return sd_card_ != nullptr ? SD_CARD_MOUNT_POINT : nullptr;
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        // Conversations, notifications and music raise the level: leave sleep.
        if (level != PowerSaveLevel::LOW_POWER && power_save_timer_ != nullptr) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        // The status bar asks every second; ten ADC conversions per second is
        // wasted work for a value that changes over minutes.
        constexpr int64_t kBatteryCacheUs = 30LL * 1000 * 1000;
        int64_t now = esp_timer_get_time();
        if (battery_percent_ < 0 || now - battery_read_us_ >= kBatteryCacheUs) {
            battery_percent_ = (int)BatterygetPercent();
            battery_read_us_ = now;
        }
        // STAT is not routed to ESP32. Keep charge current unknown; the UI
        // separately shows a bolt for a confirmed powered USB host connection.
        charging = false;
        discharging = !usb_serial_jtag_is_connected();
        level = battery_percent_;

        return true;
    }
};

DECLARE_BOARD(CustomBoard);
