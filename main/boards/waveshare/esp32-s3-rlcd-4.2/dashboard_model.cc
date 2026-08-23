#include "dashboard_model.h"

#include <algorithm>
#include <cstdio>
#include <limits>

namespace rlcd_dashboard {
namespace {

bool ParseTwoDigits(const std::string& value, size_t offset, int& output) {
    if (offset + 2 > value.size() || value[offset] < '0' || value[offset] > '9' ||
        value[offset + 1] < '0' || value[offset + 1] > '9') {
        return false;
    }
    output = (value[offset] - '0') * 10 + (value[offset + 1] - '0');
    return true;
}

bool ParseClock(const std::string& value, size_t offset, int& hour, int& minute) {
    return offset + 5 <= value.size() && value[offset + 2] == ':' &&
           ParseTwoDigits(value, offset, hour) && ParseTwoDigits(value, offset + 3, minute) &&
           hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59;
}

int DaysInMonth(int year, int month) {
    static constexpr int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) {
        return 0;
    }
    if (month != 2) {
        return kDays[month - 1];
    }
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return leap ? 29 : 28;
}

bool ParseLrcTimestamp(const std::string& tag, uint32_t& timestamp_ms) {
    const size_t colon = tag.find(':');
    if (colon == std::string::npos || colon == 0 || colon + 2 >= tag.size()) {
        return false;
    }

    uint64_t minutes = 0;
    for (size_t i = 0; i < colon; ++i) {
        if (tag[i] < '0' || tag[i] > '9') {
            return false;
        }
        minutes = minutes * 10 + static_cast<uint64_t>(tag[i] - '0');
    }

    int seconds = 0;
    if (!ParseTwoDigits(tag, colon + 1, seconds) || seconds > 59) {
        return false;
    }

    uint64_t fraction_ms = 0;
    const size_t dot = colon + 3;
    if (dot < tag.size()) {
        if (tag[dot] != '.' || dot + 1 >= tag.size()) {
            return false;
        }
        size_t digits = 0;
        for (size_t i = dot + 1; i < tag.size() && digits < 3; ++i, ++digits) {
            if (tag[i] < '0' || tag[i] > '9') {
                return false;
            }
            fraction_ms = fraction_ms * 10 + static_cast<uint64_t>(tag[i] - '0');
        }
        if (digits == 1) {
            fraction_ms *= 100;
        } else if (digits == 2) {
            fraction_ms *= 10;
        }
    }

    const uint64_t total = minutes * 60000 + static_cast<uint64_t>(seconds) * 1000 + fraction_ms;
    if (total > std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    timestamp_ms = static_cast<uint32_t>(total);
    return true;
}

}  // namespace

bool WeatherData::IsValid() const {
    return !city.empty() && !condition.empty() && temperature_c >= -100 && temperature_c <= 100 &&
           humidity_percent >= -1 && humidity_percent <= 100;
}

std::string FormatWifiStatus(bool connected, int rssi) {
    if (!connected) {
        return "Wi-Fi 未连接";
    }
    const char* quality = rssi >= -60 ? "强" : (rssi >= -75 ? "中" : "弱");
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "Wi-Fi %ddBm %s", rssi, quality);
    return buffer;
}

std::string FormatBluetoothStatus(BluetoothState state) {
    switch (state) {
        case BluetoothState::kConnected:
            return "蓝牙 已连接";
        case BluetoothState::kDisconnected:
            return "蓝牙 未连接";
        case BluetoothState::kDisabled:
        default:
            return "蓝牙 关闭";
    }
}

std::string FormatBatteryStatus(int level, bool charging) {
    level = std::clamp(level, 0, 100);
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), charging ? "电量 %d%% 充电" : "电量 %d%%", level);
    return buffer;
}

std::string FormatHomeDate(const std::tm& local_time) {
    static constexpr const char* kWeekdays[] = {"周日", "周一", "周二", "周三",
                                                "周四", "周五", "周六"};
    const int weekday = std::clamp(local_time.tm_wday, 0, 6);
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%d月%d日 %s", local_time.tm_mon + 1, local_time.tm_mday,
                  kWeekdays[weekday]);
    return buffer;
}

DashboardPage ToggleIdleDashboardPage(DashboardPage current_page) {
    return current_page == DashboardPage::kMusic ? DashboardPage::kHome : DashboardPage::kMusic;
}

DashboardPage SelectDashboardPage(AssistantUiState state, bool music_playing,
                                  DashboardPage preferred_idle_page) {
    switch (state) {
        case AssistantUiState::kConnecting:
        case AssistantUiState::kListening:
        case AssistantUiState::kSpeaking:
        case AssistantUiState::kNotifying:
            return DashboardPage::kAssistant;
        case AssistantUiState::kIdle:
            return music_playing ? DashboardPage::kMusic : preferred_idle_page;
        case AssistantUiState::kOther:
        default:
            return DashboardPage::kHome;
    }
}

bool IsValidReminderTime(const std::string& value) {
    int hour = 0;
    int minute = 0;
    if (value.size() == 5) {
        return ParseClock(value, 0, hour, minute);
    }
    if (value.size() == 16 && value[4] == '-' && value[7] == '-' && value[10] == ' ') {
        int year = 0;
        int month = 0;
        int day = 0;
        if (!ParseTwoDigits(value, 5, month) || !ParseTwoDigits(value, 8, day) ||
            !ParseClock(value, 11, hour, minute)) {
            return false;
        }
        for (size_t i = 0; i < 4; ++i) {
            if (value[i] < '0' || value[i] > '9') {
                return false;
            }
            year = year * 10 + (value[i] - '0');
        }
        return year >= 2024 && day >= 1 && day <= DaysInMonth(year, month);
    }
    return false;
}

bool IsReminderDue(const std::string& value, const std::tm& local_time) {
    if (!IsValidReminderTime(value)) {
        return false;
    }
    char clock[6];
    if (std::strftime(clock, sizeof(clock), "%H:%M", &local_time) == 0) {
        return false;
    }
    if (value.size() == 5) {
        return value == clock;
    }
    char date_time[17];
    if (std::strftime(date_time, sizeof(date_time), "%Y-%m-%d %H:%M", &local_time) == 0) {
        return false;
    }
    return value == date_time;
}

ReminderBook::ReminderBook(size_t capacity) : capacity_(capacity) {}

bool ReminderBook::Add(ReminderItem item) {
    if (items_.size() >= capacity_ || item.id.empty() || item.content.empty() ||
        (!item.time.empty() && !IsValidReminderTime(item.time))) {
        return false;
    }
    const auto duplicate =
        std::find_if(items_.begin(), items_.end(),
                     [&item](const ReminderItem& existing) { return existing.id == item.id; });
    if (duplicate != items_.end()) {
        return false;
    }
    items_.push_back(std::move(item));
    return true;
}

bool ReminderBook::RemoveById(const std::string& id) {
    const auto position = std::find_if(items_.begin(), items_.end(),
                                       [&id](const ReminderItem& item) { return item.id == id; });
    if (position == items_.end()) {
        return false;
    }
    items_.erase(position);
    return true;
}

void ReminderBook::Clear() { items_.clear(); }

std::optional<ReminderItem> ReminderBook::PopDue(const std::tm& local_time) {
    const auto position =
        std::find_if(items_.begin(), items_.end(), [&local_time](const ReminderItem& item) {
            return !item.time.empty() && IsReminderDue(item.time, local_time);
        });
    if (position == items_.end()) {
        return std::nullopt;
    }
    ReminderItem due = std::move(*position);
    items_.erase(position);
    return due;
}

std::vector<LyricLine> ParseLrc(const std::string& lrc) {
    std::vector<LyricLine> result;
    size_t line_start = 0;
    while (line_start <= lrc.size()) {
        size_t line_end = lrc.find('\n', line_start);
        if (line_end == std::string::npos) {
            line_end = lrc.size();
        }
        std::string line = lrc.substr(line_start, line_end - line_start);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        std::vector<uint32_t> timestamps;
        size_t cursor = 0;
        while (cursor < line.size() && line[cursor] == '[') {
            const size_t close = line.find(']', cursor + 1);
            if (close == std::string::npos) {
                break;
            }
            uint32_t timestamp = 0;
            if (ParseLrcTimestamp(line.substr(cursor + 1, close - cursor - 1), timestamp)) {
                timestamps.push_back(timestamp);
            }
            cursor = close + 1;
        }
        if (!timestamps.empty()) {
            const std::string text = line.substr(cursor);
            for (uint32_t timestamp : timestamps) {
                result.push_back({timestamp, text});
            }
        }

        if (line_end == lrc.size()) {
            break;
        }
        line_start = line_end + 1;
    }
    std::stable_sort(
        result.begin(), result.end(),
        [](const LyricLine& left, const LyricLine& right) { return left.time_ms < right.time_ms; });
    return result;
}

LyricWindow SelectLyricWindow(const std::vector<LyricLine>& lines, uint32_t position_ms) {
    LyricWindow window;
    if (lines.empty()) {
        return window;
    }
    if (position_ms < lines.front().time_ms) {
        window.next = lines.front().text;
        return window;
    }
    auto upper = std::upper_bound(
        lines.begin(), lines.end(), position_ms,
        [](uint32_t position, const LyricLine& line) { return position < line.time_ms; });
    const size_t current = static_cast<size_t>(std::distance(lines.begin(), upper) - 1);
    window.current = lines[current].text;
    if (current > 0) {
        window.previous = lines[current - 1].text;
    }
    if (current + 1 < lines.size()) {
        window.next = lines[current + 1].text;
    }
    return window;
}

int MusicProgressPermille(uint32_t current_ms, uint32_t total_ms) {
    if (total_ms == 0) {
        return 0;
    }
    const uint64_t value = static_cast<uint64_t>(current_ms) * 1000 / total_ms;
    return static_cast<int>(std::min<uint64_t>(value, 1000));
}

std::string FormatPlaybackTime(uint32_t milliseconds) {
    const uint32_t total_seconds = milliseconds / 1000;
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%02lu:%02lu",
                  static_cast<unsigned long>(total_seconds / 60),
                  static_cast<unsigned long>(total_seconds % 60));
    return buffer;
}

}  // namespace rlcd_dashboard
