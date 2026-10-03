#pragma once
#include "dashboard_model.h"

namespace rlcd_dashboard {
struct WeatherLocation {
    double latitude, longitude;
};
struct IpLocation {
    std::string city;
    WeatherLocation location;
};
bool IsValidWeatherCity(const std::string& city);
std::optional<std::string> ParseIpCity(const std::string& body);
std::optional<IpLocation> ParseIpLocation(const std::string& json);
std::optional<WeatherLocation> ParseWeatherLocation(const std::string& json);
std::optional<WeatherData> ParseCurrentWeather(const std::string& city, const std::string& json);
std::string WeatherCondition(int code);
std::string EncodeWeatherCity(const std::string& city);
}  // namespace rlcd_dashboard
