#include <cassert>
#include <cmath>
#include "dashboard_model.h"
using namespace rlcd_dashboard;

int main() {
    PageRouter pages;
    assert(pages.Resolve(kDeviceStateIdle, false, false, 0) == DashboardPage::kHome);
    pages.Request(DashboardPage::kCalendar);
    assert(pages.Resolve(kDeviceStateIdle, false, false, 0) == DashboardPage::kCalendar);
    assert(pages.Resolve(kDeviceStateConnecting, false, false, 0) == DashboardPage::kAssistant);
    assert(pages.Resolve(kDeviceStateSpeaking, false, false, 0) == DashboardPage::kAssistant);
    assert(pages.Resolve(kDeviceStateIdle, false, false, 0) == DashboardPage::kCalendar);
    assert(pages.Resolve(kDeviceStatePlaying, true, false, 1) == DashboardPage::kMusic);
    // Pausing returns to the desktop even if the calendar was open before music.
    assert(pages.Resolve(kDeviceStateIdle, true, true, 1) == DashboardPage::kHome);
    assert(pages.Resolve(kDeviceStateIdle, true, true, 1) == DashboardPage::kHome);
    pages.Request(DashboardPage::kCalendar);
    assert(pages.Resolve(kDeviceStateIdle, true, true, 1) == DashboardPage::kCalendar);
    // Resume can retain the same music session: it must reclaim the player page.
    assert(pages.Resolve(kDeviceStatePlaying, true, false, 1) == DashboardPage::kMusic);
    assert(pages.Resolve(kDeviceStateIdle, true, true, 1) == DashboardPage::kHome);
    assert(pages.Resolve(kDeviceStateConnecting, true, true, 1) == DashboardPage::kAssistant);
    assert(pages.Resolve(kDeviceStateListening, true, true, 1) == DashboardPage::kAssistant);
    pages.Request(DashboardPage::kCalendar);  // a voice request may show the calendar immediately
    assert(pages.Resolve(kDeviceStateSpeaking, true, true, 1) == DashboardPage::kCalendar);
    assert(pages.Resolve(kDeviceStatePlaying, true, false, 2) == DashboardPage::kMusic);
    pages.Request(DashboardPage::kHome);
    assert(pages.Resolve(kDeviceStateIdle, false, false, 0) == DashboardPage::kHome);
    assert(pages.Resolve(kDeviceStateUpgrading, false, false, 0) == DashboardPage::kAssistant);
    PageRouter connected_pages;
    connected_pages.Resolve(kDeviceStateIdle, false, false, 0);
    connected_pages.Request(DashboardPage::kCalendar);
    assert(connected_pages.Resolve(kDeviceStateListening, false, false, 0) ==
           DashboardPage::kAssistant);
    connected_pages.Request(DashboardPage::kCalendar);
    assert(connected_pages.Resolve(kDeviceStateSpeaking, false, false, 0) ==
           DashboardPage::kCalendar);
    assert(connected_pages.Resolve(kDeviceStateListening, false, false, 0) ==
           DashboardPage::kCalendar);

    // October 2026 starts on Thursday (Monday-first column 3).
    auto october = BuildCalendarMonth(2026, 10);
    assert(october.days[0] == 0 && october.days[2] == 0);
    assert(october.days[3] == 1 && october.days[33] == 31 && october.days[34] == 0);
    auto february = BuildCalendarMonth(2024, 2);
    assert(february.days[3] == 1 && february.days[31] == 29 && february.days[32] == 0);
    assert(DaysInMonth(1900, 2) == 28 && DaysInMonth(2000, 2) == 29);
    auto january = ShiftCalendarMonth(2026, 12, 1);
    assert(january.year == 2027 && january.month == 1 && january.days[4] == 1);
    auto december = ShiftCalendarMonth(2026, 1, -1);
    assert(december.year == 2025 && december.month == 12 && december.days[0] == 1);
    auto invalid = BuildCalendarMonth(2026, 13);
    for (int day : invalid.days)
        assert(day == 0);

    auto lunar = GregorianToLunar(2026, 10, 1);
    assert(lunar && lunar->year == 2026 && lunar->month == 8 && lunar->day == 21 && !lunar->leap);
    assert(FormatLunarDate(*lunar) == "丙午年 八月廿一");
    assert(LunarCellText(2026, 10, 1) == "廿一");
    assert(LunarCellText(2026, 2, 17) == "正月");
    assert(LunarCellText(2025, 7, 25) == "闰六月");
    auto leap_month = GregorianToLunar(2025, 7, 25);
    assert(leap_month && leap_month->month == 6 && leap_month->day == 1 && leap_month->leap);
    auto new_year_eve = GregorianToLunar(2026, 2, 16);
    assert(new_year_eve && new_year_eve->year == 2025 && new_year_eve->month == 12 &&
           new_year_eve->day == 29);
    assert(!GregorianToLunar(2026, 2, 29));
    assert(!GregorianToLunar(2026, 0, 1));
    assert(!GregorianToLunar(1900, 12, 31));
    assert(!GregorianToLunar(2101, 1, 1));
    assert(GregorianToLunar(1901, 1, 1) && GregorianToLunar(2100, 12, 31));
    assert(LunarCellText(2026, 10, 0).empty());
    assert(FormatLunarDate({2026, 13, 1, false}).empty());

    ReminderBook reminders(2);
    assert(reminders.Add({"r1", "", "买牛奶"}));
    assert(reminders.Add({"r2", "2026-10-01 14:30", "接孩子"}));
    assert(!reminders.Add({"r3", "", "超出容量"}));
    std::tm now{};
    now.tm_year = 126;
    now.tm_mon = 9;
    now.tm_mday = 1;
    now.tm_hour = 14;
    now.tm_min = 29;
    assert(!reminders.PopDue(now));
    now.tm_min = 30;
    auto due = reminders.PopDue(now);
    assert(due && due->id == "r2" && reminders.Items().size() == 1);
    assert(!reminders.PopDue(now));
    assert(!reminders.Add({"r4", "2026-02-29 10:00", "无效日期"}));
    assert(!reminders.Add({"r1", "", "重复编号"}));
    assert(reminders.RemoveById("r1"));
    assert(!reminders.RemoveById("r1"));
    assert(IsValidReminderTime("2024-02-29 23:59"));
    assert(!IsValidReminderTime("24:00"));

    const uint8_t sample[] = {0x66, 0x66, 0x93, 0x80, 0x00, 0xa2};
    auto room = DecodeShtc3Sample(sample, sizeof(sample));
    assert(room && std::fabs(room->temperature_c - 25.0f) < 0.01f);
    assert(std::fabs(room->humidity_percent - 50.0f) < 0.01f);
    uint8_t corrupted[] = {0x66, 0x66, 0x92, 0x80, 0x00, 0xa2};
    assert(!DecodeShtc3Sample(corrupted, sizeof(corrupted)));
    assert(!DecodeShtc3Sample(sample, 5));
    assert(!DecodeShtc3Sample(nullptr, 6));
    WeatherData weather{"上海", "晴", 25, 50, "2026-10-01 14:00"};
    assert(weather.IsValid());
    weather.humidity_percent = 101;
    assert(!weather.IsValid());
}
