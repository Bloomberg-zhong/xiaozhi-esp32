#include "custom_lcd_display.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "dashboard_store.h"
#include "dashboard_weather.h"
#include "lvgl_theme.h"

LV_FONT_DECLARE(font_rlcd_calendar_14);

namespace {
using rlcd_dashboard::DashboardPage;

void SetText(lv_obj_t* label, const std::string& text) {
    if (label && std::strcmp(lv_label_get_text(label), text.c_str()) != 0) {
        lv_label_set_text(label, text.c_str());
    }
}

void StylePage(lv_obj_t* page, int width, int height) {
    lv_obj_set_pos(page, 0, 36);
    lv_obj_set_size(page, width, height - 36);
    lv_obj_set_style_bg_color(page, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(page, lv_color_black(), 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_radius(page, 0, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t* Label(lv_obj_t* parent, int x, int y, int width, const char* text) {
    auto* label = lv_label_create(parent);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, width);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_label_set_text(label, text);
    return label;
}

void Divider(lv_obj_t* parent, int y, int width) {
    auto* line = lv_obj_create(parent);
    lv_obj_set_pos(line, 12, y);
    lv_obj_set_size(line, width - 24, 1);
    lv_obj_set_style_bg_color(line, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_pad_all(line, 0, 0);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_SCROLLABLE);
}

const char* StateText(DeviceState state) {
    switch (state) {
        case kDeviceStateIdle:
            return "小智 待命";
        case kDeviceStatePlaying:
            return "小智 播放中";
        case kDeviceStateConnecting:
            return "小智 连接中";
        case kDeviceStateListening:
            return "小智 聆听中";
        case kDeviceStateSpeaking:
            return "小智 回答中";
        case kDeviceStateNotifying:
            return "小智 提醒中";
        case kDeviceStateWifiConfiguring:
            return "小智 配网中";
        case kDeviceStateUpgrading:
            return "小智 升级中";
        default:
            return "小智 启动中";
    }
}

const char* PageName(DashboardPage page) {
    switch (page) {
        case DashboardPage::kHome:
            return "home";
        case DashboardPage::kCalendar:
            return "calendar";
        case DashboardPage::kAssistant:
            return "assistant";
        case DashboardPage::kMusic:
            return "music";
    }
    return "home";
}
}  // namespace

void CustomLcdDisplay::WrapAssistantUI() {
    auto* screen = lv_display_get_screen_active(display_);
    // Keep the official bars outside the assistant content in either chat style.
    lv_obj_set_parent(top_bar_, screen);
    lv_obj_set_pos(top_bar_, 0, 0);
    std::vector<lv_obj_t*> roots;
    for (uint32_t i = 0; i < lv_obj_get_child_count(screen); ++i) {
        auto* child = lv_obj_get_child(screen, i);
        if (child != top_bar_ && child != status_bar_ && child != low_battery_popup_) {
            roots.push_back(child);
        }
    }
    assistant_page_ = lv_obj_create(screen);
    lv_obj_set_pos(assistant_page_, 0, 0);
    lv_obj_set_size(assistant_page_, width_, height_);
    lv_obj_set_style_bg_opa(assistant_page_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(assistant_page_, 0, 0);
    lv_obj_set_style_pad_all(assistant_page_, 0, 0);
    lv_obj_remove_flag(assistant_page_, LV_OBJ_FLAG_SCROLLABLE);
    for (auto* root : roots)
        lv_obj_set_parent(root, assistant_page_);
}

void CustomLcdDisplay::SetupUI() {
    if (IsSetupUICalled())
        return;
    LcdDisplay::SetupUI();
    DisplayLockGuard lock(this);
    if (!lock)
        return;
    WrapAssistantUI();
    SetupBatteryPercentageUI();
    SetupDashboardUI();
    SetupCalendarUI();
#if CONFIG_USE_MUSIC_PLAYER
    SetupMusicUI();
#endif
    ShowPageLocked(DashboardPage::kHome);
}

void CustomLcdDisplay::SetupBatteryPercentageUI() {
    if (!battery_label_ || battery_percentage_label_)
        return;
    auto* icons = lv_obj_get_parent(battery_label_);
    if (!icons)
        return;
    // Share the existing top icon row so all pages show percentage next to the battery.
    battery_percentage_label_ = Label(icons, 0, 0, 42, "");
    lv_obj_set_style_text_font(battery_percentage_label_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(battery_percentage_label_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_move_to_index(battery_percentage_label_, lv_obj_get_index(battery_label_));
    lv_obj_set_style_pad_column(icons, 2, 0);
    lv_obj_add_flag(battery_percentage_label_, LV_OBJ_FLAG_HIDDEN);
    // Reserve the icon row's maximum width, including the optional mute icon.
    if (status_label_)
        lv_obj_set_width(status_label_, std::max(1, width_ - 240));
    if (notification_label_)
        lv_obj_set_width(notification_label_, std::max(1, width_ - 240));
}

void CustomLcdDisplay::RefreshBatteryPercentage(bool available, int level) {
    if (!battery_percentage_label_)
        return;
    if (available) {
        char text[8];
        std::snprintf(text, sizeof(text), "%d%%", std::clamp(level, 0, 100));
        SetText(battery_percentage_label_, text);
        if (lv_obj_has_flag(battery_percentage_label_, LV_OBJ_FLAG_HIDDEN))
            lv_obj_remove_flag(battery_percentage_label_, LV_OBJ_FLAG_HIDDEN);
    } else {
        SetText(battery_percentage_label_, "");
        if (!lv_obj_has_flag(battery_percentage_label_, LV_OBJ_FLAG_HIDDEN))
            lv_obj_add_flag(battery_percentage_label_, LV_OBJ_FLAG_HIDDEN);
    }
}

void CustomLcdDisplay::SetupDashboardUI() {
    auto* screen = lv_display_get_screen_active(display_);
    dashboard_page_ = lv_obj_create(screen);
    StylePage(dashboard_page_, width_, height_);
    dashboard_time_label_ = Label(dashboard_page_, 12, 6, 190, "--:--");
    lv_obj_set_style_text_font(dashboard_time_label_, &lv_font_montserrat_48, 0);
    dashboard_date_label_ = Label(dashboard_page_, 12, 66, 190, "等待校时");
    dashboard_city_label_ = Label(dashboard_page_, 218, 8, 170, "城市天气");
    dashboard_weather_label_ = Label(dashboard_page_, 218, 32, 170, "等待天气同步");
    dashboard_outdoor_label_ = Label(dashboard_page_, 218, 56, 170, "室外 --°C");
    dashboard_updated_label_ = Label(dashboard_page_, 218, 80, 170, "尚未配置城市");
    Divider(dashboard_page_, 105, width_);
    dashboard_room_label_ = Label(dashboard_page_, 12, 114, width_ - 24, "室内 --.-°C  湿度 --%");
    Divider(dashboard_page_, 142, width_);
    dashboard_memo_label_ = Label(dashboard_page_, 12, 152, 160, "备忘录 (0)");
    dashboard_ai_label_ = Label(dashboard_page_, 218, 152, 170, "小智 待命");
    dashboard_reminder_labels_[0] =
        Label(dashboard_page_, 12, 181, width_ - 24, "告诉小智：添加备忘录");
    dashboard_reminder_labels_[1] = Label(dashboard_page_, 12, 210, width_ - 24, "");
    Divider(dashboard_page_, 237, width_);
#if CONFIG_USE_MUSIC_PLAYER
    dashboard_controls_label_ =
        Label(dashboard_page_, 12, 244, width_ - 24, "长按KEY播放内存卡音乐");
#else
    Label(dashboard_page_, 12, 244, width_ - 24, "语音切换日历 · 添加备忘");
#endif
}

void CustomLcdDisplay::SetupCalendarUI() {
    calendar_page_ = lv_obj_create(lv_display_get_screen_active(display_));
    StylePage(calendar_page_, width_, height_);
    // Own compact CJK subset: dates remain readable without server-pushed glyphs.
    lv_obj_set_style_text_font(calendar_page_, &font_rlcd_calendar_14, 0);
    calendar_title_label_ = Label(calendar_page_, 12, 4, 130, "等待校时");
    lv_obj_set_style_text_align(calendar_title_label_, LV_TEXT_ALIGN_CENTER, 0);
    calendar_lunar_label_ = Label(calendar_page_, 148, 4, width_ - 160, "今日农历：等待校时");
    lv_obj_set_style_text_align(calendar_lunar_label_, LV_TEXT_ALIGN_RIGHT, 0);
    constexpr const char* weekdays[] = {"一", "二", "三", "四", "五", "六", "日"};
    const int cell_width = (width_ - 24) / 7;
    for (int column = 0; column < 7; ++column) {
        auto* day =
            Label(calendar_page_, 12 + column * cell_width, 25, cell_width, weekdays[column]);
        lv_obj_set_style_text_align(day, LV_TEXT_ALIGN_CENTER, 0);
    }
    Divider(calendar_page_, 46, width_);
    for (int i = 0; i < 42; ++i) {
        auto* day =
            Label(calendar_page_, 14 + (i % 7) * cell_width, 49 + (i / 7) * 32, cell_width - 4, "");
        lv_obj_set_height(day, 31);
        lv_obj_set_style_text_line_space(day, -3, 0);
        lv_obj_set_style_text_align(day, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_radius(day, 6, 0);
        calendar_day_labels_[i] = day;
    }
    Divider(calendar_page_, 241, width_);
    calendar_ai_label_ = Label(calendar_page_, 12, 246, 110, "小智 待命");
    Label(calendar_page_, 124, 246, width_ - 136, "语音：上月 · 下月 · 回首页");
}

void CustomLcdDisplay::ShowPageLocked(DashboardPage page) {
    if (!assistant_page_ || !dashboard_page_ || !calendar_page_)
        return;
#if CONFIG_USE_MUSIC_PLAYER
    if (page == DashboardPage::kMusic && music_page_ == nullptr)
        page = DashboardPage::kHome;
#else
    if (page == DashboardPage::kMusic)
        page = DashboardPage::kHome;
#endif
    if (active_page_initialized_ && active_page_ == page)
        return;
    lv_obj_add_flag(assistant_page_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(dashboard_page_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(calendar_page_, LV_OBJ_FLAG_HIDDEN);
#if CONFIG_USE_MUSIC_PLAYER
    lv_obj_add_flag(music_page_, LV_OBJ_FLAG_HIDDEN);
    music_page_visible_ = page == DashboardPage::kMusic;
    if (page == DashboardPage::kMusic)
        lv_obj_remove_flag(music_page_, LV_OBJ_FLAG_HIDDEN);
#endif
    lv_obj_t* visible = page == DashboardPage::kAssistant  ? assistant_page_
                        : page == DashboardPage::kCalendar ? calendar_page_
                                                           : dashboard_page_;
    if (page != DashboardPage::kMusic)
        lv_obj_remove_flag(visible, LV_OBJ_FLAG_HIDDEN);
    // Shared native network/battery/notification widgets stay above every page.
    lv_obj_move_foreground(top_bar_);
    lv_obj_move_foreground(status_bar_);
    if (low_battery_popup_)
        lv_obj_move_foreground(low_battery_popup_);
    if (gif_controller_) {
        if (page == DashboardPage::kAssistant)
            gif_controller_->Start();
        else
            gif_controller_->Stop();
    }
    active_page_ = page;
    active_page_initialized_ = true;
    ESP_LOGI("DesktopUI", "Page: %s", PageName(page));
}

void CustomLcdDisplay::RefreshDashboard() {
    if (!dashboard_page_)
        return;
    auto& app = Application::GetInstance();
    auto& store = rlcd_dashboard::DashboardStore::Instance();
    auto weather = store.GetWeather();
    auto room = store.GetRoomEnvironment();
    auto reminders = store.GetReminders();
    const auto now = std::time(nullptr);
    std::tm local{};
    const bool time_valid = localtime_r(&now, &local) && local.tm_year >= 125;
    // The model removes a due entry before posting the notification, so repeated
    // ticks and two reminders in the same minute cannot duplicate/drop it.
    auto due = time_valid ? store.PopDueReminder(local) : std::nullopt;
    if (due) {
        active_reminder_text_ = due->content;
        active_reminder_until_ = now + 60;
        app.Schedule([this, message = due->content]() {
#if CONFIG_USE_MUSIC_PLAYER
            auto& application = Application::GetInstance();
            application.GetMusicPlayer().SetWantsPlayback(false);
            application.StopMusic();
#endif
            RequestPage(DashboardPage::kHome);
            Application::GetInstance().Alert("提醒", message.c_str(), "happy",
                                             Lang::Sounds::OGG_POPUP);
        });
    }
    bool has_music = false, paused = false;
    uint32_t session_id = 0;
#if CONFIG_USE_MUSIC_PLAYER
    has_music = app.GetMusicPlayer().IsPlaying() || app.GetMusicPlayer().IsPaused();
    paused = app.GetMusicPlayer().IsPaused();
    session_id = app.GetMusicPlayer().session_id();
#endif
    int battery = 0;
    bool charging = false, discharging = false;
    bool has_battery = Board::GetInstance().GetBatteryLevel(battery, charging, discharging);
    DisplayLockGuard lock(this);
    if (!lock)
        return;
    ShowPageLocked(page_router_.Resolve(app.GetDeviceState(), has_music, paused, session_id));
#if CONFIG_USE_MUSIC_PLAYER
    SetText(dashboard_controls_label_,
            paused ? "KEY播放音乐 · 长按停止回首页" : "长按KEY播放内存卡音乐");
#endif
    SetText(dashboard_ai_label_, StateText(app.GetDeviceState()));
    SetText(calendar_ai_label_, StateText(app.GetDeviceState()));
    char text[96];
    if (time_valid) {
        std::strftime(text, sizeof(text), "%H:%M", &local);
        SetText(dashboard_time_label_, text);
        constexpr const char* weekdays[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
        std::snprintf(text, sizeof(text), "%04d-%02d-%02d %s", local.tm_year + 1900,
                      local.tm_mon + 1, local.tm_mday, weekdays[local.tm_wday]);
        SetText(dashboard_date_label_, text);
        auto month = rlcd_dashboard::ShiftCalendarMonth(local.tm_year + 1900, local.tm_mon + 1,
                                                        calendar_month_offset_);
        const int stamp = (month.year * 12 + month.month) * 32 + local.tm_mday;
        if (stamp != calendar_stamp_) {
            calendar_stamp_ = stamp;
            std::snprintf(text, sizeof(text), "%d年%d月", month.year, month.month);
            SetText(calendar_title_label_, text);
            auto lunar_today = rlcd_dashboard::GregorianToLunar(local.tm_year + 1900,
                                                                local.tm_mon + 1, local.tm_mday);
            const std::string lunar_text =
                lunar_today ? rlcd_dashboard::FormatLunarDate(*lunar_today) : "超出换算范围";
            SetText(calendar_lunar_label_, "今日农历 " + lunar_text);
            ESP_LOGI("DesktopUI", "Calendar %04d-%02d, today %s", month.year, month.month,
                     lunar_text.c_str());
            for (size_t i = 0; i < month.days.size(); ++i) {
                int day = month.days[i];
                std::snprintf(text, sizeof(text), "%d", day);
                std::string cell = day ? text : "";
                if (day) {
                    auto lunar = rlcd_dashboard::LunarCellText(month.year, month.month, day);
                    if (!lunar.empty())
                        cell += "\n" + lunar;
                }
                SetText(calendar_day_labels_[i], cell);
                bool today = month.year == local.tm_year + 1900 &&
                             month.month == local.tm_mon + 1 && day == local.tm_mday;
                lv_obj_set_style_bg_color(calendar_day_labels_[i], lv_color_black(), 0);
                lv_obj_set_style_bg_opa(calendar_day_labels_[i],
                                        today ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
                lv_obj_set_style_text_color(calendar_day_labels_[i],
                                            today ? lv_color_white() : lv_color_black(), 0);
            }
        }
    } else {
        SetText(dashboard_time_label_, "--:--");
        SetText(dashboard_date_label_, "等待网络校时");
        SetText(calendar_title_label_, "等待网络校时");
        SetText(calendar_lunar_label_, "今日农历：等待校时");
    }
    if (weather.IsValid()) {
        SetText(dashboard_city_label_, weather.city + "天气");
        SetText(dashboard_weather_label_, weather.condition);
        std::snprintf(text, sizeof(text), "室外 %d°C · %d%%RH", weather.temperature_c,
                      weather.humidity_percent);
        if (weather.humidity_percent < 0)
            std::snprintf(text, sizeof(text), "室外 %d°C", weather.temperature_c);
        SetText(dashboard_outdoor_label_, text);
        std::string updated = weather.updated_at;
        if (updated.size() >= 16)
            updated = updated.substr(5, 5) + " " + updated.substr(11, 5);
        SetText(dashboard_updated_label_, updated.empty() ? "天气已同步" : "更新 " + updated);
    } else {
        auto city = DashboardWeather::Instance().GetCity();
        SetText(dashboard_city_label_, city.empty() ? "城市天气" : city + "天气");
        SetText(dashboard_updated_label_, city.empty() && DashboardWeather::Instance().IsAutomatic()
                                              ? "等待IP定位城市"
                                              : "等待真实天气同步");
    }
    if (room) {
        std::snprintf(text, sizeof(text), "室内 %.1f°C  湿度 %.0f%%", room->temperature_c,
                      room->humidity_percent);
        SetText(dashboard_room_label_, text);
    } else
        SetText(dashboard_room_label_, "室内温湿度：传感器暂不可用");
    std::snprintf(text, sizeof(text), "备忘录 (%u)", static_cast<unsigned>(reminders.size()));
    SetText(dashboard_memo_label_, text);
    for (size_t i = 0; i < dashboard_reminder_labels_.size(); ++i) {
        std::string memo;
        if (i == 0 && now < active_reminder_until_)
            memo = "提醒：" + active_reminder_text_;
        else if (i < reminders.size())
            memo =
                (reminders[i].time.empty() ? "" : reminders[i].time + " ") + reminders[i].content;
        else if (i == 0)
            memo = "告诉小智：添加备忘录";
        SetText(dashboard_reminder_labels_[i], memo);
    }
    RefreshBatteryPercentage(has_battery, battery);
}

void CustomLcdDisplay::RequestPage(DashboardPage page) {
    {
        DisplayLockGuard lock(this);
        if (!lock)
            return;
        page_router_.Request(page);
        ShowPageLocked(page);
    }
    RefreshDashboard();
}

void CustomLcdDisplay::ToggleHomeCalendarPage() {
    RequestPage(active_page_ == DashboardPage::kCalendar ? DashboardPage::kHome
                                                         : DashboardPage::kCalendar);
}

void CustomLcdDisplay::BrowseCalendarMonth(int direction) {
    {
        DisplayLockGuard lock(this);
        if (!lock)
            return;
        calendar_month_offset_ = std::clamp(calendar_month_offset_ + direction, -120, 120);
        page_router_.Request(DashboardPage::kCalendar);
        calendar_stamp_ = -1;
    }
    RefreshDashboard();
}
