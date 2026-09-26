#include "display.h"
#include <esp_err.h>
#include <esp_log.h>
#include <cstdlib>
#include <cstring>
#include <string>
#include "application.h"
#include "assets/lang_config.h"
#include "audio_codec.h"
#include "board.h"
#include "lvgl_display/lvgl_image.h"
#include "settings.h"

#define TAG "Display"

Display::Display() {}

Display::~Display() {}

void Display::SetPreviewImage(std::unique_ptr<LvglImage> image) {}

void Display::SetStatus(const char* status) { ESP_LOGW(TAG, "SetStatus: %s", status); }

void Display::ShowNotification(const std::string& notification, int duration_ms) {
    ShowNotification(notification.c_str(), duration_ms);
}

void Display::ShowNotification(const char* notification, int duration_ms) {
    ESP_LOGW(TAG, "ShowNotification: %s", notification);
}

void Display::UpdateStatusBar(bool update_all) {}

void Display::SetEmotion(const char* emotion) { ESP_LOGW(TAG, "SetEmotion: %s", emotion); }

void Display::SetChatMessage(const char* role, const char* content) {
    ESP_LOGW(TAG, "Role:%s", role);
    ESP_LOGW(TAG, "     %s", content);
}

void Display::ClearChatMessages() {
    // Default empty implementation, override in subclasses if needed
}

void Display::SetTheme(Theme* theme) {
    current_theme_ = theme;
    Settings settings("display", true);
    settings.SetString("theme", theme->name());
}

void Display::SetPowerSaveMode(bool on) { ESP_LOGW(TAG, "SetPowerSaveMode: %d", on); }

void Display::SetMusicInfo(const char* title, const char* artist) {
    ESP_LOGW(TAG, "SetMusicInfo: %s - %s", title, artist);
}

void Display::SetMusicLyric(const char* lyric) { ESP_LOGW(TAG, "SetMusicLyric: %s", lyric); }

void Display::SetMusicProgress(uint32_t current_ms, uint32_t total_ms) {
    ESP_LOGD(TAG, "SetMusicProgress: %lu/%lu", static_cast<unsigned long>(current_ms),
             static_cast<unsigned long>(total_ms));
}

void Display::SwitchToMusicPage() {}

void Display::SwitchToWeatherPage() {}
