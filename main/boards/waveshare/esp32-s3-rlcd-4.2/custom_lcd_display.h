#ifndef __CUSTOM_LCD_DISPLAY_H__
#define __CUSTOM_LCD_DISPLAY_H__

#include <driver/gpio.h>

#include <atomic>

#include "lcd_display.h"

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
    bool landscape_ = true;
    // True from the moment a frame is handed to the SPI DMA until its
    // completion interrupt, which is what releases LVGL.
    std::atomic<bool> flush_in_flight_{false};
    void Set_ResetIOLevel(uint8_t level);
    void RLCD_SendCommand(uint8_t Reg);
    void RLCD_SendData(uint8_t Data);
    void RLCD_Sendbuffera(uint8_t* Data, int len);
    void RLCD_Reset(void);
    static void Lvgl_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* color_p);
    static bool OnColorTransferDone(esp_lcd_panel_io_handle_t panel_io,
                                    esp_lcd_panel_io_event_data_t* event_data, void* user_ctx);

#if CONFIG_USE_MUSIC_PLAYER
    lv_obj_t* music_page_ = nullptr;
    lv_obj_t* music_title_label_ = nullptr;
    lv_obj_t* music_artist_label_ = nullptr;
    lv_obj_t* music_state_label_ = nullptr;
    lv_obj_t* music_lyric_label_ = nullptr;
    lv_obj_t* music_progress_bar_ = nullptr;
    lv_obj_t* music_progress_label_ = nullptr;
    std::atomic<bool> music_page_visible_{false};
    uint32_t music_ui_session_id_ = 0;
    void SetupMusicUI();
    void RefreshMusicUI();
#endif

public:
    CustomLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                     int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                     bool swap_xy, spi_display_config_t spiconfig,
                     spi_host_device_t spi_host = SPI3_HOST);
    ~CustomLcdDisplay();
    void SetupUI() override;
    void SetStatus(const char* status) override;
    void SetChatMessage(const char* role, const char* content) override;
    void SetEmotion(const char* emotion) override;
    void UpdateStatusBar(bool update_all = false) override;
    // Switches the ST7305 controller between high power mode (fast refresh)
    // and low power mode (slow refresh, a fraction of the panel current).
    virtual void SetPowerSaveMode(bool on) override;
    void RLCD_Init();
    void RLCD_ColorClear(uint8_t color);
    void RLCD_Display();
    void RLCD_SetPixel(uint16_t x, uint16_t y, uint8_t color);
};

#endif
