#ifndef __CUSTOM_LCD_DISPLAY_H__
#define __CUSTOM_LCD_DISPLAY_H__

#include <driver/gpio.h>
#include <ctime>
#include <string>
#include "dashboard_model.h"
#include "lcd_display.h"
#include "rlcd_flush_coordinator.h"

enum ColorSelection { ColorBlack = 0, ColorWhite = 0xff };

typedef struct {
    uint8_t mosi;
    uint8_t scl;
    uint8_t dc;
    uint8_t cs;
    uint8_t rst;
} spi_display_config_t;

class CustomLcdDisplay : public LcdDisplay {
private:
    esp_lcd_panel_io_handle_t io_handle = NULL;
    uint32_t i2c_data_pdMS_TICKS = 0;
    uint32_t i2c_done_pdMS_TICKS = 0;
    const char* TAG = "CustomDisplay";
    int mosi_;
    int scl_;
    int dc_;
    int cs_;
    int rst_;
    int width_;
    int height_;
    uint8_t* DispBuffer = NULL;
    int DisplayLen;
    uint16_t (*PixelIndexLUT)[300];
    uint8_t (*PixelBitLUT)[300];
    void InitPortraitLUT();
    void InitLandscapeLUT();
    void Set_ResetIOLevel(uint8_t level);
    void RLCD_SendCommand(uint8_t Reg);
    void RLCD_SendData(uint8_t Data);
    void RLCD_Sendbuffera(uint8_t* Data, int len);
    void RLCD_Reset(void);
    static void Lvgl_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* color_p);
    static bool OnColorTransferDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*,
                                    void* user_ctx);

    RlcdFlushCoordinator flush_coordinator_;

    lv_obj_t* assistant_page_ = nullptr;
    lv_obj_t* dashboard_page_ = nullptr;
    lv_obj_t* dashboard_time_label_ = nullptr;
    lv_obj_t* dashboard_date_label_ = nullptr;
    lv_obj_t* dashboard_temperature_label_ = nullptr;
    lv_obj_t* dashboard_condition_label_ = nullptr;
    lv_obj_t* dashboard_weather_detail_label_ = nullptr;
    lv_obj_t* dashboard_reminder_time_label_ = nullptr;
    lv_obj_t* dashboard_reminder_content_label_ = nullptr;
    lv_obj_t* dashboard_bluetooth_icon_label_ = nullptr;
    lv_obj_t* dashboard_bluetooth_label_ = nullptr;
    lv_obj_t* dashboard_ai_status_label_ = nullptr;
    lv_obj_t* dashboard_memo_count_label_ = nullptr;
    lv_obj_t* music_page_ = nullptr;
    lv_obj_t* music_title_label_ = nullptr;
    lv_obj_t* music_artist_label_ = nullptr;
    lv_obj_t* music_lyric_previous_label_ = nullptr;
    lv_obj_t* music_lyric_current_label_ = nullptr;
    lv_obj_t* music_lyric_next_label_ = nullptr;
    lv_obj_t* music_progress_bar_ = nullptr;
    lv_obj_t* music_progress_label_ = nullptr;
    lv_obj_t* music_ai_status_label_ = nullptr;
    lv_timer_t* dashboard_timer_ = nullptr;
    rlcd_dashboard::DashboardPage active_page_ = rlcd_dashboard::DashboardPage::kHome;
    rlcd_dashboard::DashboardPage preferred_idle_page_ = rlcd_dashboard::DashboardPage::kHome;
    bool active_page_initialized_ = false;
    int last_reminder_minute_ = -1;
    std::string active_reminder_text_;
    time_t active_reminder_until_ = 0;

    void SetupDashboardUI();
    void SetupMusicUI();
    void WrapAssistantUI();
    void ShowPageLocked(rlcd_dashboard::DashboardPage page);
    void RefreshDashboard();
    void CheckDueReminder(const tm& local_time);
    static void DashboardTimerCallback(lv_timer_t* timer);

public:
    CustomLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                     int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                     bool swap_xy, spi_display_config_t spiconfig,
                     spi_host_device_t spi_host = SPI3_HOST);
    ~CustomLcdDisplay();
    void SetupUI() override;
    void SetStatus(const char* status) override;
    void SetMusicInfo(const char* title, const char* artist) override;
    void SetMusicLyric(const char* lyric) override;
    void SetMusicProgress(uint32_t current_ms, uint32_t total_ms) override;
    void SwitchToMusicPage() override;
    void SwitchToWeatherPage() override;
    void ToggleHomeMusicPage();
    void RLCD_Init();
    void RLCD_ColorClear(uint8_t color);
    void RLCD_Display();
    void RLCD_SetPixel(uint16_t x, uint16_t y, uint8_t color);
};

#endif
