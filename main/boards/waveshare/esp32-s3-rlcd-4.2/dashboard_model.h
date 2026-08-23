#pragma once

#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <vector>

namespace rlcd_dashboard {

enum class BluetoothState {
    kDisabled,
    kDisconnected,
    kConnected,
};

enum class AssistantUiState {
    kIdle,
    kConnecting,
    kListening,
    kSpeaking,
    kNotifying,
    kOther,
};

enum class DashboardPage {
    kHome,
    kAssistant,
    kMusic,
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
    std::string id;
    std::string time;
    std::string content;
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

struct LyricLine {
    uint32_t time_ms = 0;
    std::string text;
};

struct LyricWindow {
    std::string previous;
    std::string current;
    std::string next;
};

std::string FormatWifiStatus(bool connected, int rssi);
std::string FormatBluetoothStatus(BluetoothState state);
std::string FormatBatteryStatus(int level, bool charging);
std::string FormatHomeDate(const std::tm& local_time);
DashboardPage ToggleIdleDashboardPage(DashboardPage current_page);
DashboardPage SelectDashboardPage(AssistantUiState state, bool music_playing,
                                  DashboardPage preferred_idle_page = DashboardPage::kHome);

bool IsValidReminderTime(const std::string& value);
bool IsReminderDue(const std::string& value, const std::tm& local_time);

std::vector<LyricLine> ParseLrc(const std::string& lrc);
LyricWindow SelectLyricWindow(const std::vector<LyricLine>& lines, uint32_t position_ms);
int MusicProgressPermille(uint32_t current_ms, uint32_t total_ms);
std::string FormatPlaybackTime(uint32_t milliseconds);

}  // namespace rlcd_dashboard
