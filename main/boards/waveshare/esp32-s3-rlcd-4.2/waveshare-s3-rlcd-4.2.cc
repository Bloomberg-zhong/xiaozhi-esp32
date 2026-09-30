#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <esp_log.h>
#include "custom_lcd_display.h"
#include "wifi_board.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "codecs/box_audio_codec.h"
#include "wifi_station.h"
#include "mcp_server.h"
#include "lvgl.h"
#include "power_save_timer.h"
#include "settings.h"
#if CONFIG_USE_POMODORO
#include "pomodoro/pomodoro.h"
#endif
#if CONFIG_USE_MUSIC_PLAYER
#include "music/music_tools.h"
#endif

#include <driver/sdmmc_host.h>
#include <esp_timer.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>

#define TAG "waveshare_rlcd_4_2"

// Idle time before power saving: wake word, microphone and the CPU go to
// light sleep, and the panel switches to low power mode. Press BOOT to wake.
#define POWER_SAVE_IDLE_SECONDS 180  // Default; overridden by the wifi/sleep_seconds setting

class CustomBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Button boot_button_;
    Button user_button_;
    CustomLcdDisplay *display_;
    sdmmc_card_t* sd_card_ = nullptr;
    PowerSaveTimer* power_save_timer_ = nullptr;
    adc_oneshot_unit_handle_t adc1_handle;
    adc_cali_handle_t cali_handle;
    bool vbat_status = 0;
    int battery_percent_ = -1;
    int64_t battery_read_us_ = 0;

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

    // KEY: click pauses/resumes (the pomodoro when running, otherwise the
    // music), double click skips to the next song, triple click changes the
    // play mode, long press stops both.
    void InitializeUserButton() {
        user_button_.OnClick([this]() {
            power_save_timer_->WakeUp();
#if CONFIG_USE_POMODORO
            if (Pomodoro::GetInstance().IsActive()) {
                Pomodoro::GetInstance().TogglePause();
                return;
            }
#endif
#if CONFIG_USE_MUSIC_PLAYER
            auto& app = Application::GetInstance();
            auto& player = app.GetMusicPlayer();
            if (player.IsPlaying()) {
                app.PauseMusic();
            } else if (player.HasTrack()) {
                app.PlayMusic(false);
            }
#endif
        });
        user_button_.OnDoubleClick([this]() {
            power_save_timer_->WakeUp();
#if CONFIG_USE_MUSIC_PLAYER
            auto& app = Application::GetInstance();
            if (app.GetMusicPlayer().HasTrack()) {
                app.SkipMusic(true);
            }
#endif
        });
        user_button_.OnMultipleClick(
            [this]() {
                power_save_timer_->WakeUp();
#if CONFIG_USE_MUSIC_PLAYER
                CycleMusicPlayMode();
#endif
            },
            3);
        user_button_.OnLongPress([this]() {
            power_save_timer_->WakeUp();
#if CONFIG_USE_POMODORO
            Pomodoro::GetInstance().Stop();
#endif
#if CONFIG_USE_MUSIC_PLAYER
            Application::GetInstance().StopMusic();
#endif
        });
    }

    void InitializePowerSaveTimer() {
        Settings settings("wifi", false);
        int idle_seconds = settings.GetInt("sleep_seconds", POWER_SAVE_IDLE_SECONDS);
        power_save_timer_ = new PowerSaveTimer(240, idle_seconds, -1);
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
            "Enable or disable automatic power saving. When enabled, after the idle time the "
            "device stops listening for the wake word and sleeps to save battery; the user "
            "presses the BOOT button to wake it. Disable it when the device is on USB power "
            "and the wake word should always work.\n"
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
        display_ = new CustomLcdDisplay(NULL, NULL, RLCD_WIDTH,RLCD_HEIGHT,DISPLAY_OFFSET_X,DISPLAY_OFFSET_Y,DISPLAY_MIRROR_X,DISPLAY_MIRROR_Y,DISPLAY_SWAP_XY,spi_config);
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
            int voltage = 0; // mV
            adc_oneshot_read(adc_handle, ADC_CHANNEL_3, &raw_value);
            adc_cali_raw_to_voltage(cali_handle, raw_value, &raw_voltage);
            voltage =  raw_voltage * 3;
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
        InitializePowerSaveTimer();
   }

    virtual AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec audio_codec(
            i2c_bus_, 
            AUDIO_INPUT_SAMPLE_RATE, 
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, 
            AUDIO_I2S_GPIO_BCLK, 
            AUDIO_I2S_GPIO_WS, 
            AUDIO_I2S_GPIO_DOUT, 
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, 
            AUDIO_CODEC_ES8311_ADDR, 
            AUDIO_CODEC_ES7210_ADDR, 
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
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

    virtual bool GetBatteryLevel(int &level, bool& charging, bool& discharging) override {
        // The status bar asks every second; ten ADC conversions per second is
        // wasted work for a value that changes over minutes.
        constexpr int64_t kBatteryCacheUs = 30LL * 1000 * 1000;
        int64_t now = esp_timer_get_time();
        if (battery_percent_ < 0 || now - battery_read_us_ >= kBatteryCacheUs) {
            battery_percent_ = (int)BatterygetPercent();
            battery_read_us_ = now;
        }
        charging = false;
        discharging = !charging;
        level = battery_percent_;

        return true;
    }
};

DECLARE_BOARD(CustomBoard);