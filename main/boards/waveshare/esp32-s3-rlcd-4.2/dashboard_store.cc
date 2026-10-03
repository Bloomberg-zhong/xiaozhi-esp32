#include "dashboard_store.h"

#include <cstdio>

#include <esp_log.h>
#include <cJSON.h>

#include "settings.h"

namespace rlcd_dashboard {
namespace {

const char* TAG = "RlcdDashboardStore";
constexpr const char* kWeatherNamespace = "rlcd_dash";
constexpr const char* kReminderNamespace = "rlcd_memo";

}  // namespace

DashboardStore& DashboardStore::Instance() {
    static DashboardStore instance;
    return instance;
}

DashboardStore::DashboardStore() {
    LoadWeather();
    LoadReminders();
}

WeatherData DashboardStore::GetWeather() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return weather_;
}

bool DashboardStore::UpdateWeather(WeatherData weather) {
    if (!weather.IsValid()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    weather_ = std::move(weather);
    SaveWeatherLocked();
    return true;
}

std::optional<RoomEnvironment> DashboardStore::GetRoomEnvironment() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return room_environment_;
}

void DashboardStore::UpdateRoomEnvironment(std::optional<RoomEnvironment> environment) {
    std::lock_guard<std::mutex> lock(mutex_);
    room_environment_ = environment;
}

std::vector<ReminderItem> DashboardStore::GetReminders() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return reminders_.Items();
}

std::optional<ReminderItem> DashboardStore::AddReminder(const std::string& time,
                                                        const std::string& content) {
    std::lock_guard<std::mutex> lock(mutex_);
    char id[16];
    std::snprintf(id, sizeof(id), "r%lu", static_cast<unsigned long>(++reminder_sequence_));
    ReminderItem item{id, time, content};
    if (!reminders_.Add(item)) {
        --reminder_sequence_;
        return std::nullopt;
    }
    SaveRemindersLocked();
    return item;
}

bool DashboardStore::RemoveReminder(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!reminders_.RemoveById(id)) {
        return false;
    }
    SaveRemindersLocked();
    return true;
}

void DashboardStore::ClearReminders() {
    std::lock_guard<std::mutex> lock(mutex_);
    reminders_.Clear();
    SaveRemindersLocked();
}

std::optional<ReminderItem> DashboardStore::PopDueReminder(const std::tm& local_time) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto due = reminders_.PopDue(local_time);
    if (due.has_value()) {
        SaveRemindersLocked();
    }
    return due;
}

void DashboardStore::LoadWeather() {
    Settings settings(kWeatherNamespace, false);
    WeatherData loaded;
    loaded.city = settings.GetString("city");
    loaded.condition = settings.GetString("condition");
    loaded.temperature_c = settings.GetInt("temp", 0);
    loaded.humidity_percent = settings.GetInt("humidity", -1);
    loaded.updated_at = settings.GetString("updated");
    if (loaded.IsValid()) {
        weather_ = std::move(loaded);
    }
}

void DashboardStore::LoadReminders() {
    Settings settings(kReminderNamespace, false);
    reminder_sequence_ = static_cast<uint32_t>(settings.GetInt("sequence", 0));
    const std::string json = settings.GetString("items");
    if (json.empty()) {
        return;
    }

    cJSON* root = cJSON_Parse(json.c_str());
    if (root == nullptr || !cJSON_IsArray(root)) {
        ESP_LOGW(TAG, "Ignoring invalid reminder JSON");
        cJSON_Delete(root);
        return;
    }
    cJSON* entry = nullptr;
    cJSON_ArrayForEach (entry, root) {
        cJSON* id = cJSON_GetObjectItemCaseSensitive(entry, "id");
        cJSON* time = cJSON_GetObjectItemCaseSensitive(entry, "time");
        cJSON* content = cJSON_GetObjectItemCaseSensitive(entry, "content");
        if (!cJSON_IsString(id) || !cJSON_IsString(time) || !cJSON_IsString(content)) {
            continue;
        }
        reminders_.Add({id->valuestring, time->valuestring, content->valuestring});
    }
    cJSON_Delete(root);
}

void DashboardStore::SaveWeatherLocked() const {
    Settings settings(kWeatherNamespace, true);
    settings.SetString("city", weather_.city);
    settings.SetString("condition", weather_.condition);
    settings.SetInt("temp", weather_.temperature_c);
    settings.SetInt("humidity", weather_.humidity_percent);
    settings.SetString("updated", weather_.updated_at);
}

void DashboardStore::SaveRemindersLocked() const {
    cJSON* root = cJSON_CreateArray();
    for (const auto& item : reminders_.Items()) {
        cJSON* entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "id", item.id.c_str());
        cJSON_AddStringToObject(entry, "time", item.time.c_str());
        cJSON_AddStringToObject(entry, "content", item.content.c_str());
        cJSON_AddItemToArray(root, entry);
    }
    char* json = cJSON_PrintUnformatted(root);
    Settings settings(kReminderNamespace, true);
    settings.SetString("items", json != nullptr ? json : "[]");
    settings.SetInt("sequence", static_cast<int32_t>(reminder_sequence_));
    cJSON_free(json);
    cJSON_Delete(root);
}

}  // namespace rlcd_dashboard
