#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <vector>

#include "device_state.h"

namespace rlcd_dashboard {

inline constexpr size_t kMaxWeatherCityBytes = 48;
inline constexpr size_t kMaxWeatherConditionBytes = 48;
inline constexpr size_t kMaxWeatherTimestampBytes = 64;
inline constexpr size_t kMaxReminderContentBytes = 192;

enum class DashboardPage { kHome, kCalendar, kAssistant, kMusic };

// Explicit page requests remain visible through the current reply, but a new
// wake or song takes over. The idle preference is home/calendar only.
class PageRouter {
public:
    void Request(DashboardPage page);
    DashboardPage Resolve(DeviceState state, bool has_music, bool paused, uint32_t session_id);

private:
    DashboardPage idle_page_ = DashboardPage::kHome;
    std::optional<DashboardPage> requested_page_;
    uint32_t music_session_id_ = 0;
    DeviceState previous_state_ = kDeviceStateStarting;
};

struct WeatherData {
    std::string city;
    std::string condition;
    int temperature_c = 0;
    int humidity_percent = -1;
    std::string updated_at;
    bool IsValid() const;
};

struct ReminderItem {
    std::string id, time, content;
};

class ReminderBook {
public:
    explicit ReminderBook(size_t capacity);
    bool Add(ReminderItem item);
    bool RemoveById(const std::string& id);
    void Clear();
    std::optional<ReminderItem> PopDue(const std::tm& local_time);
    const std::vector<ReminderItem>& Items() const { return items_; }

private:
    size_t capacity_;
    std::vector<ReminderItem> items_;
};

bool IsValidReminderTime(const std::string& value);
bool IsReminderDue(const std::string& value, const std::tm& local_time);

struct CalendarMonth {
    int year = 0, month = 0;
    std::array<int, 42> days{};  // Monday first; zero is an empty cell.
};
int DaysInMonth(int year, int month);
CalendarMonth BuildCalendarMonth(int year, int month);
CalendarMonth ShiftCalendarMonth(int year, int month, int offset);

struct LunarDate {
    int year = 0, month = 0, day = 0;
    bool leap = false;
};
// Offline HKO conversion data covers Gregorian 1901-01-01 through 2100-12-31.
std::optional<LunarDate> GregorianToLunar(int year, int month, int day);
std::string FormatLunarDate(const LunarDate& date);
std::string LunarCellText(int year, int month, int day);

struct RoomEnvironment {
    float temperature_c = 0;
    float humidity_percent = 0;
};
std::optional<RoomEnvironment> DecodeShtc3Sample(const uint8_t* data, size_t size);

}  // namespace rlcd_dashboard
