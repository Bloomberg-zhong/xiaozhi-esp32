#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "dashboard_model.h"

namespace rlcd_dashboard {

class DashboardStore {
public:
    static DashboardStore& Instance();

    WeatherData GetWeather() const;
    bool UpdateWeather(WeatherData weather);
    std::optional<RoomEnvironment> GetRoomEnvironment() const;
    void UpdateRoomEnvironment(std::optional<RoomEnvironment> environment);

    std::vector<ReminderItem> GetReminders() const;
    std::optional<ReminderItem> AddReminder(const std::string& time, const std::string& content);
    bool RemoveReminder(const std::string& id);
    void ClearReminders();
    std::optional<ReminderItem> PopDueReminder(const std::tm& local_time);

private:
    DashboardStore();

    void LoadWeather();
    void LoadReminders();
    void SaveWeatherLocked() const;
    void SaveRemindersLocked() const;

    mutable std::mutex mutex_;
    WeatherData weather_;
    std::optional<RoomEnvironment> room_environment_;
    ReminderBook reminders_{8};
    uint32_t reminder_sequence_ = 0;
};

}  // namespace rlcd_dashboard
