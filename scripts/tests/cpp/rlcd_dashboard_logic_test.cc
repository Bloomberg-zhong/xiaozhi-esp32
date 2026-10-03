#include <cassert>
#include <cmath>
#include "dashboard_model.h"
using namespace rlcd_dashboard;

int main() {
    PageRouter pages;
    assert(pages.Resolve(kDeviceStateIdle, false, false, 0) == DashboardPage::kHome);
    assert(pages.Resolve(kDeviceStateConnecting, false, false, 0) == DashboardPage::kAssistant);
    assert(pages.Resolve(kDeviceStateSpeaking, false, false, 0) == DashboardPage::kAssistant);
    assert(pages.Resolve(kDeviceStateIdle, false, false, 0) == DashboardPage::kHome);
    assert(pages.Resolve(kDeviceStatePlaying, true, false, 1) == DashboardPage::kMusic);
    // Pausing returns to the desktop; resuming reclaims the player page.
    assert(pages.Resolve(kDeviceStateIdle, true, true, 1) == DashboardPage::kHome);
    assert(pages.Resolve(kDeviceStatePlaying, true, false, 1) == DashboardPage::kMusic);
    assert(pages.Resolve(kDeviceStateIdle, true, true, 1) == DashboardPage::kHome);
    assert(pages.Resolve(kDeviceStateConnecting, true, true, 1) == DashboardPage::kAssistant);
    assert(pages.Resolve(kDeviceStateListening, true, true, 1) == DashboardPage::kAssistant);
    // A new song takes over from the desktop.
    assert(pages.Resolve(kDeviceStatePlaying, true, false, 2) == DashboardPage::kMusic);
    // An explicit "go home" request stays until a wake or a new song takes over.
    pages.Request(DashboardPage::kHome);
    assert(pages.Resolve(kDeviceStatePlaying, true, false, 2) == DashboardPage::kHome);
    assert(pages.Resolve(kDeviceStateListening, true, false, 2) == DashboardPage::kAssistant);
    assert(pages.Resolve(kDeviceStatePlaying, true, false, 3) == DashboardPage::kMusic);
    assert(pages.Resolve(kDeviceStateIdle, false, false, 0) == DashboardPage::kHome);
    assert(pages.Resolve(kDeviceStateUpgrading, false, false, 0) == DashboardPage::kAssistant);

    assert(DaysInMonth(2024, 2) == 29 && DaysInMonth(2026, 2) == 28 && DaysInMonth(2026, 13) == 0);

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
