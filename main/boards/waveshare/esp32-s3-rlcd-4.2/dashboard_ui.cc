#include "custom_lcd_display.h"

#include <material_symbols.h>
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "dashboard_model.h"
#include "dashboard_store.h"
#include "lvgl_theme.h"

#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
#include "blufi.h"
#endif

namespace {

constexpr int kScreenWidth = 400;
constexpr int kScreenHeight = 300;

void AddDivider(lv_obj_t* parent, int x, int y, int width, int height) {
    auto* divider = lv_obj_create(parent);
    lv_obj_set_pos(divider, x, y);
    lv_obj_set_size(divider, width, height);
    lv_obj_set_style_bg_color(divider, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(divider, 0, 0);
    lv_obj_set_style_radius(divider, 0, 0);
    lv_obj_set_style_pad_all(divider, 0, 0);
    lv_obj_remove_flag(divider, LV_OBJ_FLAG_SCROLLABLE);
}

const char* DeviceStateText(DeviceState state) {
    switch (state) {
        case kDeviceStateListening:
            return "聆听中";
        case kDeviceStateSpeaking:
            return "回答中";
        case kDeviceStateNotifying:
            return "提醒中";
        case kDeviceStateConnecting:
            return "连接中";
        case kDeviceStateWifiConfiguring:
            return "配网中";
        case kDeviceStateUpgrading:
            return "升级中";
        case kDeviceStateIdle:
            return "待命";
        default:
            return "启动中";
    }
}

rlcd_dashboard::AssistantUiState ToAssistantUiState(DeviceState state) {
    switch (state) {
        case kDeviceStateConnecting:
            return rlcd_dashboard::AssistantUiState::kConnecting;
        case kDeviceStateListening:
            return rlcd_dashboard::AssistantUiState::kListening;
        case kDeviceStateSpeaking:
            return rlcd_dashboard::AssistantUiState::kSpeaking;
        case kDeviceStateNotifying:
            return rlcd_dashboard::AssistantUiState::kNotifying;
        case kDeviceStateIdle:
        case kDeviceStateUnknown:
            return rlcd_dashboard::AssistantUiState::kIdle;
        default:
            return rlcd_dashboard::AssistantUiState::kOther;
    }
}

const char* DashboardPageName(rlcd_dashboard::DashboardPage page) {
    switch (page) {
        case rlcd_dashboard::DashboardPage::kAssistant:
            return "assistant";
        case rlcd_dashboard::DashboardPage::kMusic:
            return "music";
        case rlcd_dashboard::DashboardPage::kHome:
        default:
            return "home";
    }
}

}  // namespace

void CustomLcdDisplay::SetupUI() {
    LcdDisplay::SetupUI();
    DisplayLockGuard lock(this);

    WrapAssistantUI();
    SetupDashboardUI();
    SetupMusicUI();
    ShowPageLocked(rlcd_dashboard::DashboardPage::kHome);
}

void CustomLcdDisplay::WrapAssistantUI() {
    auto* screen = lv_screen_active();
    const uint32_t assistant_root_count = lv_obj_get_child_count(screen);
    std::vector<lv_obj_t*> assistant_roots;
    assistant_roots.reserve(assistant_root_count);
    for (uint32_t index = 0; index < assistant_root_count; ++index) {
        assistant_roots.push_back(lv_obj_get_child(screen, static_cast<int32_t>(index)));
    }

    assistant_page_ = lv_obj_create(screen);
    lv_obj_set_pos(assistant_page_, 0, 0);
    lv_obj_set_size(assistant_page_, kScreenWidth, kScreenHeight);
    lv_obj_set_style_bg_opa(assistant_page_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(assistant_page_, 0, 0);
    lv_obj_set_style_pad_all(assistant_page_, 0, 0);
    lv_obj_remove_flag(assistant_page_, LV_OBJ_FLAG_SCROLLABLE);

    // The official network, mute, battery, and low-battery UI is shared by the home and assistant
    // pages. Move only assistant-specific roots under the assistant page.
    for (auto* child : assistant_roots) {
        if (child != nullptr && child != top_bar_ && child != status_bar_ &&
            child != low_battery_popup_) {
            lv_obj_set_parent(child, assistant_page_);
        }
    }
}

void CustomLcdDisplay::SetupDashboardUI() {
    auto* screen = lv_screen_active();
    auto* theme = static_cast<LvglTheme*>(current_theme_);
    auto* icon_font = theme->icon_font()->font();

    dashboard_page_ = lv_obj_create(screen);
    lv_obj_set_pos(dashboard_page_, 0, 0);
    lv_obj_set_size(dashboard_page_, kScreenWidth, kScreenHeight);
    lv_obj_set_style_bg_color(dashboard_page_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(dashboard_page_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dashboard_page_, 0, 0);
    lv_obj_set_style_radius(dashboard_page_, 0, 0);
    lv_obj_set_style_pad_all(dashboard_page_, 0, 0);
    lv_obj_remove_flag(dashboard_page_, LV_OBJ_FLAG_SCROLLABLE);

    dashboard_date_label_ = lv_label_create(dashboard_page_);
    lv_obj_set_pos(dashboard_date_label_, 100, 8);
    lv_obj_set_width(dashboard_date_label_, 200);
    lv_obj_set_style_text_align(dashboard_date_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(dashboard_date_label_, "--月--日 ---");

    dashboard_time_label_ = lv_label_create(dashboard_page_);
    lv_obj_set_pos(dashboard_time_label_, 16, 52);
    lv_obj_set_width(dashboard_time_label_, 180);
    lv_obj_set_style_text_font(dashboard_time_label_, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_align(dashboard_time_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(dashboard_time_label_, "--:--");

    AddDivider(dashboard_page_, 210, 48, 1, 72);

    dashboard_temperature_label_ = lv_label_create(dashboard_page_);
    lv_obj_set_pos(dashboard_temperature_label_, 228, 46);
    lv_obj_set_width(dashboard_temperature_label_, 156);
    lv_obj_set_style_text_font(dashboard_temperature_label_, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_align(dashboard_temperature_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(dashboard_temperature_label_, "--°");

    dashboard_condition_label_ = lv_label_create(dashboard_page_);
    lv_obj_set_pos(dashboard_condition_label_, 228, 101);
    lv_obj_set_width(dashboard_condition_label_, 156);
    lv_obj_set_style_text_align(dashboard_condition_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(dashboard_condition_label_, LV_LABEL_LONG_DOT);
    lv_label_set_text(dashboard_condition_label_, "等待天气");

    AddDivider(dashboard_page_, 12, 133, 376, 1);
    AddDivider(dashboard_page_, 210, 148, 1, 96);

    auto* weather_icon = lv_label_create(dashboard_page_);
    lv_obj_set_pos(weather_icon, 14, 145);
    lv_obj_set_style_text_font(weather_icon, icon_font, 0);
    lv_label_set_text(weather_icon, MATERIAL_SYMBOLS_DEVICE_THERMOSTAT);

    auto* weather_title = lv_label_create(dashboard_page_);
    lv_obj_set_pos(weather_title, 50, 151);
    lv_label_set_text(weather_title, "天气详情");

    dashboard_weather_detail_label_ = lv_label_create(dashboard_page_);
    lv_obj_set_pos(dashboard_weather_detail_label_, 16, 184);
    lv_obj_set_width(dashboard_weather_detail_label_, 178);
    lv_label_set_long_mode(dashboard_weather_detail_label_, LV_LABEL_LONG_WRAP);
    lv_label_set_text(dashboard_weather_detail_label_, "等待真实天气数据");

    auto* reminder_icon = lv_label_create(dashboard_page_);
    lv_obj_set_pos(reminder_icon, 220, 145);
    lv_obj_set_style_text_font(reminder_icon, icon_font, 0);
    lv_label_set_text(reminder_icon, MATERIAL_SYMBOLS_NOTIFICATIONS);

    auto* reminder_title = lv_label_create(dashboard_page_);
    lv_obj_set_pos(reminder_title, 256, 151);
    lv_label_set_text(reminder_title, "下一条提醒");

    dashboard_reminder_time_label_ = lv_label_create(dashboard_page_);
    lv_obj_set_pos(dashboard_reminder_time_label_, 222, 184);
    lv_obj_set_width(dashboard_reminder_time_label_, 160);
    lv_label_set_long_mode(dashboard_reminder_time_label_, LV_LABEL_LONG_DOT);
    lv_label_set_text(dashboard_reminder_time_label_, "暂无提醒");

    dashboard_reminder_content_label_ = lv_label_create(dashboard_page_);
    lv_obj_set_pos(dashboard_reminder_content_label_, 222, 210);
    lv_obj_set_width(dashboard_reminder_content_label_, 160);
    lv_label_set_long_mode(dashboard_reminder_content_label_, LV_LABEL_LONG_DOT);
    lv_label_set_text(dashboard_reminder_content_label_, "可以告诉小智添加");

    AddDivider(dashboard_page_, 12, 258, 376, 1);

    dashboard_bluetooth_icon_label_ = lv_label_create(dashboard_page_);
    lv_obj_set_pos(dashboard_bluetooth_icon_label_, 14, 263);
    lv_obj_set_style_text_font(dashboard_bluetooth_icon_label_, icon_font, 0);
    lv_label_set_text(dashboard_bluetooth_icon_label_, MATERIAL_SYMBOLS_BLUETOOTH);

    dashboard_bluetooth_label_ = lv_label_create(dashboard_page_);
    lv_obj_set_pos(dashboard_bluetooth_label_, 50, 270);
    lv_obj_set_width(dashboard_bluetooth_label_, 78);
    lv_label_set_long_mode(dashboard_bluetooth_label_, LV_LABEL_LONG_DOT);
    lv_label_set_text(dashboard_bluetooth_label_, "关闭");

    dashboard_ai_status_label_ = lv_label_create(dashboard_page_);
    lv_obj_set_pos(dashboard_ai_status_label_, 145, 270);
    lv_obj_set_width(dashboard_ai_status_label_, 110);
    lv_obj_set_style_text_align(dashboard_ai_status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(dashboard_ai_status_label_, "小智 启动中");

    dashboard_memo_count_label_ = lv_label_create(dashboard_page_);
    lv_obj_set_pos(dashboard_memo_count_label_, 275, 270);
    lv_obj_set_width(dashboard_memo_count_label_, 110);
    lv_obj_set_style_text_align(dashboard_memo_count_label_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(dashboard_memo_count_label_, "0 条备忘");

    RefreshDashboard();
    dashboard_timer_ = lv_timer_create(DashboardTimerCallback, 1000, this);
}

void CustomLcdDisplay::RefreshDashboard() {
    if (dashboard_time_label_ == nullptr || dashboard_date_label_ == nullptr) {
        return;
    }

    const auto now = std::time(nullptr);
    std::tm local_time = {};
    if (localtime_r(&now, &local_time) == nullptr) {
        lv_label_set_text(dashboard_time_label_, "--:--");
        lv_label_set_text(dashboard_date_label_, "等待校时");
        return;
    }

    char clock[8];
    std::strftime(clock, sizeof(clock), "%H:%M", &local_time);
    lv_label_set_text(dashboard_time_label_, clock);
    const std::string date = rlcd_dashboard::FormatHomeDate(local_time);
    lv_label_set_text(dashboard_date_label_, date.c_str());

    rlcd_dashboard::BluetoothState bluetooth_state = rlcd_dashboard::BluetoothState::kDisabled;
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
    auto& blufi = Blufi::GetInstance();
    bluetooth_state = blufi.IsInitialized()
                          ? (blufi.IsBleConnected() ? rlcd_dashboard::BluetoothState::kConnected
                                                    : rlcd_dashboard::BluetoothState::kDisconnected)
                          : rlcd_dashboard::BluetoothState::kDisabled;
#endif
    const char* bluetooth_status =
        bluetooth_state == rlcd_dashboard::BluetoothState::kConnected      ? "已连接"
        : bluetooth_state == rlcd_dashboard::BluetoothState::kDisconnected ? "未连接"
                                                                           : "关闭";
    lv_label_set_text(dashboard_bluetooth_label_, bluetooth_status);

    const auto weather = rlcd_dashboard::DashboardStore::Instance().GetWeather();
    if (weather.IsValid()) {
        char temperature[16];
        char weather_detail[160];
        std::snprintf(temperature, sizeof(temperature), "%d°", weather.temperature_c);
        lv_label_set_text(dashboard_temperature_label_, temperature);
        lv_label_set_text(dashboard_condition_label_, weather.condition.c_str());
        if (weather.humidity_percent >= 0) {
            std::snprintf(weather_detail, sizeof(weather_detail), "%s · 湿度 %d%%\n更新 %s",
                          weather.city.c_str(), weather.humidity_percent,
                          weather.updated_at.empty() ? "刚刚" : weather.updated_at.c_str());
        } else {
            std::snprintf(weather_detail, sizeof(weather_detail), "%s\n更新 %s",
                          weather.city.c_str(),
                          weather.updated_at.empty() ? "刚刚" : weather.updated_at.c_str());
        }
        lv_label_set_text(dashboard_weather_detail_label_, weather_detail);
    } else {
        lv_label_set_text(dashboard_temperature_label_, "--°");
        lv_label_set_text(dashboard_condition_label_, "等待天气");
        lv_label_set_text(dashboard_weather_detail_label_, "请让小智同步\n真实天气数据");
    }

    const auto reminders = rlcd_dashboard::DashboardStore::Instance().GetReminders();
    if (!active_reminder_text_.empty() && now < active_reminder_until_) {
        lv_label_set_text(dashboard_reminder_time_label_, "提醒");
        lv_label_set_text(dashboard_reminder_content_label_, active_reminder_text_.c_str());
    } else {
        active_reminder_text_.clear();
        if (reminders.empty()) {
            lv_label_set_text(dashboard_reminder_time_label_, "暂无提醒");
            lv_label_set_text(dashboard_reminder_content_label_, "可以告诉小智添加");
        } else {
            lv_label_set_text(dashboard_reminder_time_label_, reminders.front().time.empty()
                                                                  ? "备忘"
                                                                  : reminders.front().time.c_str());
            lv_label_set_text(dashboard_reminder_content_label_, reminders.front().content.c_str());
        }
    }

    const std::string ai_status =
        std::string("小智 ") + DeviceStateText(Application::GetInstance().GetDeviceState());
    lv_label_set_text(dashboard_ai_status_label_, ai_status.c_str());
    if (music_ai_status_label_ != nullptr) {
        lv_label_set_text(music_ai_status_label_, Application::GetInstance().IsMusicPlaying()
                                                      ? "播放中 · 语音唤醒可打断"
                                                      : "待命 · 说出歌名即可播放");
    }
    char memo_count[32];
    std::snprintf(memo_count, sizeof(memo_count), "%u 条备忘",
                  static_cast<unsigned>(reminders.size()));
    lv_label_set_text(dashboard_memo_count_label_, memo_count);
    CheckDueReminder(local_time);
}

void CustomLcdDisplay::CheckDueReminder(const std::tm& local_time) {
    if (local_time.tm_min == last_reminder_minute_) {
        return;
    }
    last_reminder_minute_ = local_time.tm_min;
    auto due = rlcd_dashboard::DashboardStore::Instance().PopDueReminder(local_time);
    if (!due.has_value()) {
        return;
    }
    active_reminder_text_ = due->content;
    active_reminder_until_ = std::time(nullptr) + 60;
    Application::GetInstance().Schedule([message = due->content]() {
        auto& app = Application::GetInstance();
        app.StopMusicPlayback(false);
        Board::GetInstance().GetDisplay()->SwitchToWeatherPage();
        app.Alert("提醒", message.c_str(), "happy", Lang::Sounds::OGG_POPUP);
    });
}

void CustomLcdDisplay::SetStatus(const char* status) {
    LcdDisplay::SetStatus(status);

    auto& application = Application::GetInstance();
    const auto page =
        rlcd_dashboard::SelectDashboardPage(ToAssistantUiState(application.GetDeviceState()),
                                            application.IsMusicPlaying(), preferred_idle_page_);
    DisplayLockGuard lock(this);
    ShowPageLocked(page);
}

void CustomLcdDisplay::ShowPageLocked(rlcd_dashboard::DashboardPage page) {
    if (assistant_page_ == nullptr || dashboard_page_ == nullptr || music_page_ == nullptr) {
        return;
    }
    if (active_page_initialized_ && active_page_ == page) {
        return;
    }

    lv_obj_add_flag(assistant_page_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(dashboard_page_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(music_page_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
    switch (page) {
        case rlcd_dashboard::DashboardPage::kAssistant:
            lv_obj_remove_flag(assistant_page_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(top_bar_);
            lv_obj_move_foreground(status_bar_);
            break;
        case rlcd_dashboard::DashboardPage::kMusic:
            lv_obj_remove_flag(music_page_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(top_bar_);
            break;
        case rlcd_dashboard::DashboardPage::kHome:
        default:
            lv_obj_remove_flag(dashboard_page_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(top_bar_);
            break;
    }
    lv_obj_move_foreground(low_battery_popup_);
    active_page_ = page;
    active_page_initialized_ = true;
    ESP_LOGI(TAG, "Show %s page", DashboardPageName(page));
}

void CustomLcdDisplay::SwitchToMusicPage() {
    DisplayLockGuard lock(this);
    preferred_idle_page_ = rlcd_dashboard::DashboardPage::kMusic;
    ShowPageLocked(rlcd_dashboard::DashboardPage::kMusic);
}

void CustomLcdDisplay::SwitchToWeatherPage() {
    DisplayLockGuard lock(this);
    preferred_idle_page_ = rlcd_dashboard::DashboardPage::kHome;
    ShowPageLocked(rlcd_dashboard::DashboardPage::kHome);
}

void CustomLcdDisplay::ToggleHomeMusicPage() {
    DisplayLockGuard lock(this);
    preferred_idle_page_ = rlcd_dashboard::ToggleIdleDashboardPage(preferred_idle_page_);
    ShowPageLocked(preferred_idle_page_);
}

void CustomLcdDisplay::DashboardTimerCallback(lv_timer_t* timer) {
    auto* display = static_cast<CustomLcdDisplay*>(lv_timer_get_user_data(timer));
    display->RefreshDashboard();
}
