#include <cassert>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>
#include "dashboard_weather_model.h"
// Compile the production class and task, replacing only hardware, network and NVS dependencies.
#define private public
#include "dashboard_weather.h"
#undef private

std::map<std::string, std::string> settings_values;
struct Settings {
    Settings(const char*, bool) {}
    std::string GetString(const char* key) { return settings_values[key]; }
    bool GetBool(const char* key, bool fallback) {
        return settings_values.count(key) ? settings_values[key] == "1" : fallback;
    }
    void SetString(const char* key, const std::string& value) { settings_values[key] = value; }
    void SetBool(const char* key, bool value) { settings_values[key] = value ? "1" : "0"; }
};
namespace rlcd_dashboard {
struct DashboardStore {
    std::vector<WeatherData> published;
    static DashboardStore& Instance() {
        static DashboardStore store;
        return store;
    }
    WeatherData GetWeather() { return {}; }
    bool UpdateWeather(const WeatherData& value) {
        published.push_back(value);
        return true;
    }
};
}  // namespace rlcd_dashboard
struct Application {
    static Application& GetInstance() {
        static Application app;
        return app;
    }
    DeviceState GetDeviceState() { return kDeviceStateIdle; }
};
int64_t fake_time = 0;
int iterations = 0, loop_limit = 1;
int ip_requests = 0, fallback_requests = 0, geo_requests = 0, weather_requests = 0;
bool primary_available = true;
std::function<void(DashboardWeather*)> during_forecast;
int64_t esp_timer_get_time() { return fake_time; }
void xTaskNotifyGive(void*) {}
void ulTaskNotifyTake(bool, int) {
    if (++iterations >= loop_limit)
        throw std::runtime_error("task completed");
    fake_time += 30LL * 60 * 1000000;
}
#define pdTRUE true
#define pdMS_TO_TICKS(ms) (ms)
#define ESP_LOGI(tag, ...) (void)(tag)
#define ESP_LOGW(tag, ...) (void)(tag)
constexpr const char* TAG = "test";
constexpr int64_t kRefreshUs = 30LL * 60 * 1000000;
constexpr int64_t kRetryUs = 5LL * 60 * 1000000;
constexpr int64_t kLocationRefreshUs = 6LL * 60 * 60 * 1000000;

bool DashboardWeather::Fetch(const std::string& url, std::string& body) {
    if (url.find("myip.ipip.net") != std::string::npos) {
        ++ip_requests;
        body = "当前 IP：1.2.3.4  来自于：中国 江苏 苏州 电信\n";
        return primary_available;
    }
    if (url.find("ipwho.is") != std::string::npos) {
        ++fallback_requests;
        body = R"({"success":true,"city":"苏州","latitude":31.3,"longitude":120.6})";
        return true;
    }
    if (url.find("geocoding-api") != std::string::npos) {
        ++geo_requests;
        body = R"({"results":[{"latitude":31.3,"longitude":120.6}]})";
        return true;
    }
    assert(url.find("forecast") != std::string::npos);
    ++weather_requests;
    if (during_forecast)
        during_forecast(this);
    body =
        R"({"current":{"temperature_2m":24.8,"relative_humidity_2m":55,"weather_code":2,"time":"2026-10-01T14:15"}})";
    return true;
}

// PRODUCTION_METHODS

void Run(DashboardWeather& weather, int loops) {
    iterations = 0;
    loop_limit = loops;
    try {
        DashboardWeather::TaskEntry(&weather);
    } catch (const std::runtime_error&) {
    }
}
void Reset() {
    settings_values.clear();
    rlcd_dashboard::DashboardStore::Instance().published.clear();
    fake_time = 0;
    ip_requests = fallback_requests = geo_requests = weather_requests = 0;
    primary_available = true;
    during_forecast = nullptr;
}
int main() {
    Reset();
    DashboardWeather automatic;
    Run(automatic, 13);  // Six hours include one IP cache refresh after initial location.
    assert(automatic.GetCity() == "苏州" && automatic.IsAutomatic());
    auto& published = rlcd_dashboard::DashboardStore::Instance().published;
    assert(published.size() == 13 && published.back().city == "苏州");
    assert(ip_requests == 2 && geo_requests == 1 && fallback_requests == 0);

    Reset();
    primary_available = false;
    DashboardWeather fallback;
    Run(fallback, 1);
    assert(fallback.GetCity() == "苏州" && published.size() == 1);
    assert(fallback_requests == 1 && geo_requests == 0);

    Reset();
    DashboardWeather changed;
    changed.Configure("上海");
    during_forecast = [](DashboardWeather* service) { service->Configure("上海"); };
    Run(changed, 1);
    assert(
        published.empty());  // Same city, newer configuration: the old request must be discarded.

    Reset();
    DashboardWeather switched;
    switched.Configure("上海");
    during_forecast = [](DashboardWeather* service) { service->Configure("auto"); };
    Run(switched, 1);
    assert(switched.IsAutomatic() && switched.GetCity().empty() && published.empty());
}
