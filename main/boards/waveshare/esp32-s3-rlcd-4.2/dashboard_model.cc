#include "dashboard_model.h"

#include <algorithm>
#include <cstdio>

namespace rlcd_dashboard {
namespace {
struct LunarYearEncoding {
    uint16_t january1;
    uint16_t month_lengths;
};
constexpr LunarYearEncoding kLunarYears[] = {
#include "dashboard_lunar_data.inc"
};
constexpr const char* kLunarMonths[] = {"正月", "二月", "三月", "四月", "五月", "六月",
                                        "七月", "八月", "九月", "十月", "冬月", "腊月"};
constexpr const char* kLunarDays[] = {
    "初一", "初二", "初三", "初四", "初五", "初六", "初七", "初八", "初九", "初十",
    "十一", "十二", "十三", "十四", "十五", "十六", "十七", "十八", "十九", "二十",
    "廿一", "廿二", "廿三", "廿四", "廿五", "廿六", "廿七", "廿八", "廿九", "三十"};

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

}  // namespace

bool WeatherData::IsValid() const {
    return !city.empty() && city.size() <= kMaxWeatherCityBytes && !condition.empty() &&
           condition.size() <= kMaxWeatherConditionBytes &&
           updated_at.size() <= kMaxWeatherTimestampBytes && temperature_c >= -100 &&
           temperature_c <= 100 && humidity_percent >= -1 && humidity_percent <= 100;
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
        item.content.size() > kMaxReminderContentBytes ||
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

void PageRouter::Request(DashboardPage page) {
    if (page == DashboardPage::kHome || page == DashboardPage::kCalendar)
        idle_page_ = page;
    requested_page_ = page;
}

DashboardPage PageRouter::Resolve(DeviceState state, bool has_music, bool paused,
                                  uint32_t session_id) {
    const bool new_listening =
        state == kDeviceStateListening && previous_state_ != kDeviceStateListening &&
        previous_state_ != kDeviceStateSpeaking && previous_state_ != kDeviceStateConnecting;
    const bool resumed_music =
        has_music && state == kDeviceStatePlaying && previous_state_ != kDeviceStatePlaying;
    if (state == kDeviceStateConnecting || new_listening || resumed_music ||
        (has_music && session_id != music_session_id_)) {
        requested_page_.reset();
    }
    previous_state_ = state;
    music_session_id_ = has_music ? session_id : 0;
    if (state == kDeviceStateStarting || state == kDeviceStateActivating ||
        state == kDeviceStateWifiConfiguring || state == kDeviceStateUpgrading ||
        state == kDeviceStateFatalError)
        return DashboardPage::kAssistant;
    if (requested_page_)
        return *requested_page_;
    if (state == kDeviceStateListening || state == kDeviceStateSpeaking ||
        state == kDeviceStateConnecting || state == kDeviceStateNotifying) {
        return DashboardPage::kAssistant;
    }
    if (has_music && state == kDeviceStatePlaying) {
        return DashboardPage::kMusic;
    }
    // Pause retains the track and position, but frees the screen for the desk.
    if (has_music && state == kDeviceStateIdle && paused)
        return DashboardPage::kHome;
    return idle_page_;
}

int DaysInMonth(int year, int month) {
    constexpr int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12)
        return 0;
    bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    return month == 2 && leap ? 29 : days[month - 1];
}

CalendarMonth BuildCalendarMonth(int year, int month) {
    CalendarMonth result;
    if (year < 1900 || year > 2199 || month < 1 || month > 12)
        return result;
    result.year = year;
    result.month = month;
    // Gregorian weekday calculation; independent of process timezone/DST.
    constexpr int offsets[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    int y = year - (month < 3 ? 1 : 0);
    int sunday_first = (y + y / 4 - y / 100 + y / 400 + offsets[month - 1] + 1) % 7;
    int first = (sunday_first + 6) % 7;
    for (int day = 1; day <= DaysInMonth(year, month); ++day)
        result.days[first + day - 1] = day;
    return result;
}

CalendarMonth ShiftCalendarMonth(int year, int month, int offset) {
    if (month < 1 || month > 12 || offset < -120 || offset > 120)
        return {};
    int absolute = year * 12 + month - 1 + offset;
    if (absolute < 1900 * 12 || absolute >= 2200 * 12)
        return {};
    return BuildCalendarMonth(absolute / 12, absolute % 12 + 1);
}

std::optional<LunarDate> GregorianToLunar(int year, int month, int day) {
    if (year < 1901 || year > 2100 || month < 1 || month > 12 || day < 1 ||
        day > DaysInMonth(year, month))
        return std::nullopt;
    const auto& encoded = kLunarYears[year - 1901];
    int lunar_month = encoded.january1 & 15;
    int remaining = ((encoded.january1 >> 4) & 31) - 1 + day - 1;
    int leap_index = (encoded.january1 >> 9) & 15;
    int lunar_year = year - 1;
    for (int m = 1; m < month; ++m)
        remaining += DaysInMonth(year, m);
    for (int index = 0; index < 14; ++index) {
        const int length = 29 + ((encoded.month_lengths >> index) & 1);
        if (remaining < length)
            return LunarDate{lunar_year, lunar_month, remaining + 1, index == leap_index};
        remaining -= length;
        if (index + 1 != leap_index) {
            lunar_month = lunar_month % 12 + 1;
            if (lunar_month == 1)
                ++lunar_year;
        }
    }
    return std::nullopt;
}

std::string FormatLunarDate(const LunarDate& date) {
    if (date.year < 1900 || date.year > 2100 || date.month < 1 || date.month > 12 || date.day < 1 ||
        date.day > 30)
        return "";
    constexpr const char* stems[] = {"甲", "乙", "丙", "丁", "戊", "己", "庚", "辛", "壬", "癸"};
    constexpr const char* branches[] = {"子", "丑", "寅", "卯", "辰", "巳",
                                        "午", "未", "申", "酉", "戌", "亥"};
    return std::string(stems[(date.year - 4) % 10]) + branches[(date.year - 4) % 12] + "年 " +
           (date.leap ? "闰" : "") + kLunarMonths[date.month - 1] + kLunarDays[date.day - 1];
}

std::string LunarCellText(int year, int month, int day) {
    auto lunar = GregorianToLunar(year, month, day);
    if (!lunar)
        return "";
    return lunar->day == 1 ? std::string(lunar->leap ? "闰" : "") + kLunarMonths[lunar->month - 1]
                           : kLunarDays[lunar->day - 1];
}

std::optional<RoomEnvironment> DecodeShtc3Sample(const uint8_t* data, size_t size) {
    if (data == nullptr || size != 6)
        return std::nullopt;
    for (int start : {0, 3}) {
        uint8_t crc = 0xff;
        for (int index = start; index < start + 2; ++index) {
            crc ^= data[index];
            for (int bit = 0; bit < 8; ++bit) {
                crc = static_cast<uint8_t>((crc << 1) ^ ((crc & 0x80) ? 0x31 : 0));
            }
        }
        if (crc != data[start + 2])
            return std::nullopt;
    }
    const uint16_t temperature = (data[0] << 8) | data[1];
    const uint16_t humidity = (data[3] << 8) | data[4];
    return RoomEnvironment{-45.0f + 175.0f * temperature / 65536.0f, 100.0f * humidity / 65536.0f};
}
}  // namespace rlcd_dashboard
