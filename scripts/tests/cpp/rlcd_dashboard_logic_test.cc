#include "dashboard_model.h"
#include "music_gateway_model.h"

#include <cassert>
#include <ctime>
#include <string>

using namespace rlcd_dashboard;

namespace {

std::tm MakeLocalTime(int year, int month, int day, int hour, int minute) {
    std::tm value{};
    value.tm_year = year - 1900;
    value.tm_mon = month - 1;
    value.tm_mday = day;
    value.tm_hour = hour;
    value.tm_min = minute;
    value.tm_isdst = -1;
    std::mktime(&value);
    return value;
}

void TestDeviceStatusFormatting() {
    assert(FormatWifiStatus(false, 0) == "Wi-Fi 未连接");
    assert(FormatWifiStatus(true, -48) == "Wi-Fi -48dBm 强");
    assert(FormatWifiStatus(true, -70) == "Wi-Fi -70dBm 中");
    assert(FormatWifiStatus(true, -85) == "Wi-Fi -85dBm 弱");

    assert(FormatBluetoothStatus(BluetoothState::kDisabled) == "蓝牙 关闭");
    assert(FormatBluetoothStatus(BluetoothState::kDisconnected) == "蓝牙 未连接");
    assert(FormatBluetoothStatus(BluetoothState::kConnected) == "蓝牙 已连接");

    assert(FormatBatteryStatus(105, true) == "电量 100% 充电");
    assert(FormatBatteryStatus(-2, false) == "电量 0%");
}

void TestDashboardPageSelection() {
    assert(SelectDashboardPage(AssistantUiState::kIdle, false) == DashboardPage::kHome);
    assert(SelectDashboardPage(AssistantUiState::kIdle, true) == DashboardPage::kMusic);
    assert(SelectDashboardPage(AssistantUiState::kConnecting, true) == DashboardPage::kAssistant);
    assert(SelectDashboardPage(AssistantUiState::kListening, false) == DashboardPage::kAssistant);
    assert(SelectDashboardPage(AssistantUiState::kSpeaking, false) == DashboardPage::kAssistant);
    assert(SelectDashboardPage(AssistantUiState::kNotifying, false) == DashboardPage::kAssistant);
    assert(SelectDashboardPage(AssistantUiState::kOther, false) == DashboardPage::kHome);
}

void TestManualMusicPageSelection() {
    assert(ToggleIdleDashboardPage(DashboardPage::kHome) == DashboardPage::kMusic);
    assert(ToggleIdleDashboardPage(DashboardPage::kMusic) == DashboardPage::kHome);

    assert(SelectDashboardPage(AssistantUiState::kIdle, false, DashboardPage::kMusic) ==
           DashboardPage::kMusic);
    assert(SelectDashboardPage(AssistantUiState::kIdle, true, DashboardPage::kHome) ==
           DashboardPage::kMusic);
    assert(SelectDashboardPage(AssistantUiState::kListening, false, DashboardPage::kMusic) ==
           DashboardPage::kAssistant);
}

void TestWeatherValidation() {
    WeatherData weather;
    assert(!weather.IsValid());
    weather.city = "上海";
    weather.condition = "多云";
    weather.temperature_c = 26;
    weather.updated_at = "2026-08-22 22:10";
    assert(weather.IsValid());
    weather.humidity_percent = 101;
    assert(!weather.IsValid());

    weather.humidity_percent = 50;
    weather.city = std::string(kMaxWeatherCityBytes + 1, 'x');
    assert(!weather.IsValid());
    weather.city = "上海";
    weather.condition = std::string(kMaxWeatherConditionBytes + 1, 'x');
    assert(!weather.IsValid());
    weather.condition = "多云";
    weather.updated_at = std::string(kMaxWeatherTimestampBytes + 1, 'x');
    assert(!weather.IsValid());
}

void TestHomeDateFormatting() {
    const auto saturday = MakeLocalTime(2026, 8, 22, 7, 30);
    assert(FormatHomeDate(saturday) == "8月22日 周六");

    const auto sunday = MakeLocalTime(2026, 8, 23, 7, 30);
    assert(FormatHomeDate(sunday) == "8月23日 周日");
}

void TestReminderTimeAndDueMatching() {
    assert(IsValidReminderTime("07:30"));
    assert(IsValidReminderTime("23:59"));
    assert(!IsValidReminderTime("7:30"));
    assert(!IsValidReminderTime("24:00"));
    assert(!IsValidReminderTime("12:60"));
    assert(!IsValidReminderTime("2026-02-29 08:00"));
    assert(IsValidReminderTime("2028-02-29 08:00"));

    const auto now = MakeLocalTime(2026, 8, 22, 7, 30);
    assert(IsReminderDue("07:30", now));
    assert(IsReminderDue("2026-08-22 07:30", now));
    assert(!IsReminderDue("2026-08-23 07:30", now));
    assert(!IsReminderDue("07:31", now));
}

void TestReminderBookBoundsAndOneShotPop() {
    ReminderBook book(2);
    assert(!book.Add({"bad", "not-a-time", "无效"}));
    assert(!book.Add({"long-id", "07:30", std::string(kMaxReminderContentBytes + 1, 'x')}));
    assert(book.Add({"one", "07:30", "开会"}));
    assert(book.Add({"two", "", "买牛奶"}));
    assert(!book.Add({"three", "08:00", "超限"}));
    assert(book.Items().size() == 2);

    const auto now = MakeLocalTime(2026, 8, 22, 7, 30);
    const auto due = book.PopDue(now);
    assert(due.has_value());
    assert(due->content == "开会");
    assert(book.Items().size() == 1);
    assert(!book.PopDue(now).has_value());
    assert(book.RemoveById("two"));
    assert(book.Items().empty());
}

void TestLrcParsingAndWindowSelection() {
    const std::string lrc =
        "[00:10.00]第一句\n"
        "[00:20.50][00:30.500]副歌\r\n"
        "[ar:歌手]\n"
        "[00:05.5]开场\n";

    const auto lines = ParseLrc(lrc);
    assert(lines.size() == 4);
    assert(lines[0].time_ms == 5500);
    assert(lines[0].text == "开场");
    assert(lines[2].time_ms == 20500);
    assert(lines[3].time_ms == 30500);

    const auto before_first = SelectLyricWindow(lines, 1000);
    assert(before_first.current.empty());
    assert(before_first.next == "开场");

    const auto middle = SelectLyricWindow(lines, 21000);
    assert(middle.previous == "第一句");
    assert(middle.current == "副歌");
    assert(middle.next == "副歌");
}

void TestMusicProgressClamping() {
    assert(MusicProgressPermille(12000, 0) == 0);
    assert(MusicProgressPermille(0, 60000) == 0);
    assert(MusicProgressPermille(30000, 60000) == 500);
    assert(MusicProgressPermille(70000, 60000) == 1000);
    assert(FormatPlaybackTime(61000) == "01:01");
}

void TestMusicGatewayUrlConstruction() {
    assert(IsValidMusicGatewayBaseUrl("http://music.local:8080"));
    assert(IsValidMusicGatewayBaseUrl("https://music.example.com/api-root/"));
    assert(!IsValidMusicGatewayBaseUrl("ftp://music.example.com"));
    assert(!IsValidMusicGatewayBaseUrl("https://"));
    assert(!IsValidMusicGatewayBaseUrl("https://user:pass@music.example.com"));
    assert(!IsValidMusicGatewayBaseUrl("https://music.example.com?token=secret"));
    assert(
        !IsValidMusicGatewayBaseUrl("https://" + std::string(kMaxMusicGatewayBaseUrlBytes, 'x')));
    assert(IsValidMusicSearchQuery("稻香 周杰伦"));
    assert(!IsValidMusicSearchQuery(std::string(kMaxMusicSearchQueryBytes + 1, 'x')));
    assert(NormalizeMusicGatewayBaseUrl("http://music.local:8080///") == "http://music.local:8080");

    assert(UrlEncode("周杰伦 稻香") == "%E5%91%A8%E6%9D%B0%E4%BC%A6%20%E7%A8%BB%E9%A6%99");
    assert(BuildMusicGatewaySearchUrl("http://music.local:8080/", "稻香", "netease") ==
           "http://music.local:8080/api/v1/music/search?q=%E7%A8%BB%E9%A6%99&type=song&"
           "sources=netease");
    assert(BuildMusicGatewaySearchUrl("http://music.local:8080", "Hello Adele", "all") ==
           "http://music.local:8080/api/v1/music/search?q=Hello%20Adele&type=song");
}

void TestMusicGatewayPlaybackUrls() {
    MusicGatewaySong song;
    song.id = "12345";
    song.source = "netease";
    song.name = "稻香";
    song.artist = "周杰伦";
    song.album = "魔杰座";
    song.duration_seconds = 223;
    song.extra_json = "{\"quality\":\"standard\"}";
    assert(song.IsValid());

    MusicGatewaySong oversized = song;
    oversized.name = std::string(kMaxMusicSongTextBytes + 1, 'x');
    assert(!oversized.IsValid());

    const std::string query =
        "id=12345&source=netease&name=%E7%A8%BB%E9%A6%99&artist=%E5%91%A8%E6%9D%B0%E4%BC%A6"
        "&album=%E9%AD%94%E6%9D%B0%E5%BA%A7&duration=223&extra=%7B%22quality%22%3A%22standard%"
        "22%7D";
    assert(BuildMusicGatewayStreamUrl("http://music.local:8080", song) ==
           "http://music.local:8080/api/v1/music/stream?" + query);
    assert(BuildMusicGatewayLyricUrl("http://music.local:8080", song) ==
           "http://music.local:8080/music/lyric?" + query);
    assert(BuildMusicGatewayInspectUrl("http://music.local:8080", song) ==
           "http://music.local:8080/api/v1/music/inspect?" + query);
    assert(BuildMusicGatewaySwitchUrl("http://music.local:8080", song) ==
           "http://music.local:8080/api/v1/music/switch?name=%E7%A8%BB%E9%A6%99&artist=%E5%"
           "91%A8%E6%9D%B0%E4%BC%A6&source=netease&duration=223");
}

}  // namespace

int main() {
    TestDeviceStatusFormatting();
    TestDashboardPageSelection();
    TestManualMusicPageSelection();
    TestWeatherValidation();
    TestHomeDateFormatting();
    TestReminderTimeAndDueMatching();
    TestReminderBookBoundsAndOneShotPop();
    TestLrcParsingAndWindowSelection();
    TestMusicProgressClamping();
    TestMusicGatewayUrlConstruction();
    TestMusicGatewayPlaybackUrls();
    return 0;
}
