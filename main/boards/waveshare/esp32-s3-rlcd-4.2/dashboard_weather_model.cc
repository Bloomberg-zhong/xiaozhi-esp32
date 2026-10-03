#include "dashboard_weather_model.h"
#include <cJSON.h>
#include <cmath>
#include <memory>
#include <sstream>

namespace rlcd_dashboard {
namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
bool BoundedJson(const std::string& json) {
    if (json.size() > 4096)
        return false;
    int depth = 0;
    bool quoted = false, escaped = false;
    for (char ch : json) {
        if (quoted) {
            if (escaped)
                escaped = false;
            else if (ch == '\\')
                escaped = true;
            else if (ch == '"')
                quoted = false;
        } else if (ch == '"') {
            quoted = true;
        } else if (ch == '{' || ch == '[') {
            if (++depth > 16)
                return false;
        } else if (ch == '}' || ch == ']') {
            if (--depth < 0)
                return false;
        }
    }
    return depth == 0 && !quoted;
}
bool Number(const cJSON* item, double min, double max) {
    return cJSON_IsNumber(item) && std::isfinite(item->valuedouble) && item->valuedouble >= min &&
           item->valuedouble <= max;
}
}  // namespace

bool IsValidWeatherCity(const std::string& city) {
    if (city.empty() || city.size() > kMaxWeatherCityBytes || city.front() == ' ' ||
        city.back() == ' ' || city == "未知" || city == "unknown" || city == "N/A" ||
        city == "局域网" || city == "电信" || city == "联通" || city == "移动")
        return false;
    for (unsigned char ch : city) {
        if (ch < 32 || ch == 127 || ch == '<' || ch == '>')
            return false;
    }
    return true;
}

std::optional<std::string> ParseIpCity(const std::string& body) {
    if (body.size() > 512 || body.find("当前 IP：") != 0 ||
        body.find_first_of("<>\0", 0, 3) != std::string::npos)
        return std::nullopt;
    const std::string marker = "来自于：";
    auto start = body.find(marker);
    if (start == std::string::npos)
        return std::nullopt;
    std::istringstream region(body.substr(start + marker.size()));
    std::string country, province, city;
    // IPIP's Chinese result has country/province/city fields. Other formats use the JSON fallback.
    if (!(region >> country >> province) || country != "中国")
        return std::nullopt;
    if (province == "北京" || province == "上海" || province == "天津" || province == "重庆" ||
        province == "香港" || province == "澳门")
        city = province;
    else if (!(region >> city))
        return std::nullopt;
    return IsValidWeatherCity(city) ? std::optional<std::string>(city) : std::nullopt;
}

std::optional<IpLocation> ParseIpLocation(const std::string& json) {
    if (!BoundedJson(json))
        return std::nullopt;
    Json root(cJSON_ParseWithLengthOpts(json.c_str(), json.size() + 1, nullptr, true),
              cJSON_Delete);
    auto* success = cJSON_GetObjectItemCaseSensitive(root.get(), "success");
    auto* city = cJSON_GetObjectItemCaseSensitive(root.get(), "city");
    auto* latitude = cJSON_GetObjectItemCaseSensitive(root.get(), "latitude");
    auto* longitude = cJSON_GetObjectItemCaseSensitive(root.get(), "longitude");
    if (!cJSON_IsTrue(success) || !cJSON_IsString(city) || !IsValidWeatherCity(city->valuestring) ||
        !Number(latitude, -90, 90) || !Number(longitude, -180, 180))
        return std::nullopt;
    return IpLocation{city->valuestring, {latitude->valuedouble, longitude->valuedouble}};
}

std::optional<WeatherLocation> ParseWeatherLocation(const std::string& json) {
    if (!BoundedJson(json))
        return std::nullopt;
    Json root(cJSON_Parse(json.c_str()), cJSON_Delete);
    auto* results = cJSON_GetObjectItemCaseSensitive(root.get(), "results");
    auto* first = cJSON_GetArrayItem(results, 0);
    auto* latitude = cJSON_GetObjectItemCaseSensitive(first, "latitude");
    auto* longitude = cJSON_GetObjectItemCaseSensitive(first, "longitude");
    if (!Number(latitude, -90, 90) || !Number(longitude, -180, 180))
        return std::nullopt;
    return WeatherLocation{latitude->valuedouble, longitude->valuedouble};
}

std::string WeatherCondition(int code) {
    switch (code) {
        case 0:
            return "晴";
        case 1:
            return "晴间多云";
        case 2:
            return "多云";
        case 3:
            return "阴";
        case 45:
        case 48:
            return "雾";
        case 51:
        case 53:
        case 55:
            return "毛毛雨";
        case 56:
        case 57:
        case 66:
        case 67:
            return "冻雨";
        case 61:
            return "小雨";
        case 63:
            return "中雨";
        case 65:
            return "大雨";
        case 71:
            return "小雪";
        case 73:
            return "中雪";
        case 75:
        case 77:
            return "大雪";
        case 80:
        case 81:
        case 82:
            return "阵雨";
        case 85:
        case 86:
            return "阵雪";
        case 95:
        case 97:
            return "雷雨";
        case 96:
        case 99:
            return "雷雨冰雹";
        default:
            return "";
    }
}

std::optional<WeatherData> ParseCurrentWeather(const std::string& city, const std::string& json) {
    if (!BoundedJson(json))
        return std::nullopt;
    Json root(cJSON_Parse(json.c_str()), cJSON_Delete);
    auto* current = cJSON_GetObjectItemCaseSensitive(root.get(), "current");
    auto* temperature = cJSON_GetObjectItemCaseSensitive(current, "temperature_2m");
    auto* humidity = cJSON_GetObjectItemCaseSensitive(current, "relative_humidity_2m");
    auto* code = cJSON_GetObjectItemCaseSensitive(current, "weather_code");
    auto* time = cJSON_GetObjectItemCaseSensitive(current, "time");
    if (!Number(temperature, -100, 100) || !Number(humidity, 0, 100) || !Number(code, 0, 99) ||
        code->valuedouble != code->valueint || !cJSON_IsString(time))
        return std::nullopt;
    std::string stamp = time->valuestring;
    if (stamp.size() != 16 || stamp[10] != 'T')
        return std::nullopt;
    stamp[10] = ' ';
    if (!IsValidReminderTime(stamp))
        return std::nullopt;
    WeatherData weather{city, WeatherCondition(code->valueint),
                        static_cast<int>(std::lround(temperature->valuedouble)),
                        static_cast<int>(std::lround(humidity->valuedouble)), time->valuestring};
    return weather.IsValid() ? std::optional<WeatherData>(std::move(weather)) : std::nullopt;
}

std::string EncodeWeatherCity(const std::string& city) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (unsigned char byte : city) {
        if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '.' ||
            byte == '~') {
            result += byte;
        } else {
            result += '%';
            result += hex[byte >> 4];
            result += hex[byte & 15];
        }
    }
    return result;
}
}  // namespace rlcd_dashboard
