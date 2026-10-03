#include "dashboard_weather.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <array>
#include <cstdio>
#include "application.h"
#include "board.h"
#include "dashboard_store.h"
#include "dashboard_weather_model.h"
#include "settings.h"

namespace {
constexpr const char* TAG = "DesktopWeather";
constexpr size_t kMaxBody = 4096;
constexpr int64_t kRefreshUs = 30LL * 60 * 1000000;
constexpr int64_t kRetryUs = 5LL * 60 * 1000000;
constexpr int64_t kLocationRefreshUs = 6LL * 60 * 60 * 1000000;
}  // namespace

DashboardWeather& DashboardWeather::Instance() {
    static DashboardWeather weather;
    return weather;
}

DashboardWeather::DashboardWeather() {
    Settings settings("rlcd_dash", false);
    // Existing installations start using IP location. An explicit manual override remains
    // persistent.
    automatic_ = settings.GetBool("wc_auto", true);
    if (!automatic_) {
        city_ = settings.GetString("wc_city");
        if (city_.empty())
            city_ = rlcd_dashboard::DashboardStore::Instance().GetWeather().city;
    }
}

std::string DashboardWeather::GetCity() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return city_;
}

bool DashboardWeather::IsAutomatic() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return automatic_;
}

bool DashboardWeather::Configure(const std::string& city) {
    if (!rlcd_dashboard::IsValidWeatherCity(city))
        return false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        automatic_ = city == "auto" || city == "自动" || city == "IP" || city == "ip";
        city_ = automatic_ ? "" : city;
        ++revision_;
        Settings settings("rlcd_dash", true);
        settings.SetBool("wc_auto", automatic_);
        if (!automatic_)
            settings.SetString("wc_city", city);
    }
    if (task_)
        xTaskNotifyGive(task_);
    return true;
}

bool DashboardWeather::Start() {
    return task_ || xTaskCreate(TaskEntry, "desktop_weather", 6144, this, 1, &task_) == pdPASS;
}

bool DashboardWeather::Fetch(const std::string& url, std::string& body) {
    if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle)
        return false;
    auto* network = Board::GetInstance().GetNetwork();
    if (!network)
        return false;
    // Dedicated connection; never reuse the voice/music stream's connection id.
    auto http = network->CreateHttp(6);
    if (!http)
        return false;
    http->SetTimeout(4000);
    http->SetKeepAlive(false);
    http->SetHeader("Accept-Encoding", "identity");
    const auto deadline = esp_timer_get_time() + 12 * 1000000;
    if (!http->Open("GET", url)) {
        http->Close();
        return false;
    }
    auto status = http->GetStatusCode();
    if (!status || *status != 200 || http->GetBodyLength() > kMaxBody) {
        http->Close();
        return false;
    }
    body.clear();
    std::array<char, 512> buffer{};
    while (esp_timer_get_time() < deadline &&
           Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
        auto read = http->Read(buffer.data(), buffer.size());
        if (!read || *read < 0 || body.size() + *read > kMaxBody)
            break;
        if (*read == 0) {
            http->Close();
            return true;
        }
        body.append(buffer.data(), *read);
    }
    http->Close();
    return false;
}

void DashboardWeather::TaskEntry(void* context) {
    auto* self = static_cast<DashboardWeather*>(context);
    std::string coordinate_city;
    std::optional<rlcd_dashboard::WeatherLocation> coordinates;
    uint32_t last_revision = UINT32_MAX;
    int64_t next_refresh = 0, next_location = 0;
    for (;;) {
        std::string city;
        bool automatic;
        uint32_t revision;
        {
            std::lock_guard<std::mutex> lock(self->mutex_);
            city = self->city_;
            automatic = self->automatic_;
            revision = self->revision_;
        }
        if (revision != last_revision) {
            last_revision = revision;
            next_refresh = 0;
            next_location = 0;
            coordinates.reset();
        }
        if ((automatic || !city.empty()) && esp_timer_get_time() >= next_refresh &&
            Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
            std::string body;
            bool located = !automatic || esp_timer_get_time() < next_location;
            if (automatic && !located) {
                std::optional<std::string> ip_city;
                std::optional<rlcd_dashboard::WeatherLocation> ip_coordinates;
                if (self->Fetch("https://myip.ipip.net/", body))
                    ip_city = rlcd_dashboard::ParseIpCity(body);
                if (!ip_city &&
                    self->Fetch(
                        "https://ipwho.is/?lang=zh-CN&fields=success,city,latitude,longitude",
                        body)) {
                    auto location = rlcd_dashboard::ParseIpLocation(body);
                    if (location) {
                        ip_city = location->city;
                        ip_coordinates = location->location;
                    }
                }
                if (ip_city) {
                    std::lock_guard<std::mutex> lock(self->mutex_);
                    if (self->revision_ == revision && self->automatic_) {
                        self->city_ = city = *ip_city;
                        if (ip_coordinates) {
                            coordinates = ip_coordinates;
                            coordinate_city = city;
                        }
                        next_location = esp_timer_get_time() + kLocationRefreshUs;
                        located = true;
                        ESP_LOGI(TAG, "IP city located: %s", city.c_str());
                    }
                }
            }
            if (located && (!coordinates || coordinate_city != city)) {
                coordinates.reset();
                if (self->Fetch(
                        "https://geocoding-api.open-meteo.com/v1/search?count=1&language=zh&name=" +
                            rlcd_dashboard::EncodeWeatherCity(city),
                        body)) {
                    coordinates = rlcd_dashboard::ParseWeatherLocation(body);
                    if (coordinates)
                        coordinate_city = city;
                }
            }
            bool success = false;
            if (located && coordinates && coordinate_city == city &&
                Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
                char url[320];
                std::snprintf(
                    url, sizeof(url),
                    "https://api.open-meteo.com/v1/forecast?latitude=%.5f&longitude=%.5f&current="
                    "temperature_2m,relative_humidity_2m,weather_code&timezone=auto",
                    coordinates->latitude, coordinates->longitude);
                if (self->Fetch(url, body)) {
                    auto weather = rlcd_dashboard::ParseCurrentWeather(city, body);
                    // A mode/city change during the request must not publish the previous result.
                    std::lock_guard<std::mutex> lock(self->mutex_);
                    if (weather && self->revision_ == revision && self->city_ == city) {
                        success =
                            rlcd_dashboard::DashboardStore::Instance().UpdateWeather(*weather);
                        ESP_LOGI(TAG, "%s %s %d C %d%% at %s", city.c_str(),
                                 weather->condition.c_str(), weather->temperature_c,
                                 weather->humidity_percent, weather->updated_at.c_str());
                    }
                }
            }
            if (!success)
                ESP_LOGW(TAG, "Weather refresh deferred or unavailable; keeping last data");
            next_refresh = esp_timer_get_time() + (success ? kRefreshUs : kRetryUs);
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(30000));
    }
}
