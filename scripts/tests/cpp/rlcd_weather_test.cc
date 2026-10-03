#include <cassert>
#include "dashboard_weather_model.h"
using namespace rlcd_dashboard;

int main() {
    auto ip_city = ParseIpCity("当前 IP：58.209.39.160  来自于：中国 江苏 苏州  电信\n");
    assert(ip_city && *ip_city == "苏州");
    assert(ParseIpCity("当前 IP：1.2.3.4  来自于：中国 上海 上海  电信\r\n") == "上海");
    assert(!ParseIpCity("当前 IP：1.2.3.4  来自于：中国 江苏 电信\n"));
    assert(!ParseIpCity("当前 IP：1.2.3.4  来自于：美国 California Los Angeles"));
    assert(!ParseIpCity("<html>中国 江苏 苏州</html>"));
    assert(!ParseIpCity(std::string(4097, 'a')));
    assert(!ParseIpCity("当前 IP：1.2.3.4  来自于：中国 江苏 未知  电信"));
    auto ip_location =
        ParseIpLocation(R"({"success":true,"city":"苏州","latitude":31.3,"longitude":120.6})");
    assert(ip_location && ip_location->city == "苏州" && ip_location->location.longitude > 120);
    assert(!ParseIpLocation(R"({"success":false,"city":"苏州","latitude":31,"longitude":120})"));
    assert(!ParseIpLocation(R"({"success":true,"city":"","latitude":31,"longitude":120})"));
    assert(!ParseIpLocation(R"({"success":true,"city":"苏州","latitude":91,"longitude":120})"));
    assert(!ParseIpLocation(R"({"success":true,"city":"苏州\n","latitude":31,"longitude":120})"));
    assert(!ParseIpLocation(R"({"success":true,"city":"苏州","latitude":"31","longitude":120})"));
    assert(
        !ParseIpLocation(R"({"success":true,"city":"苏州","latitude":31,"longitude":120}garbage)"));
    auto city = ParseWeatherLocation(R"({"results":[{"latitude":31.22,"longitude":121.46}]})");
    assert(city && city->latitude > 31 && city->longitude > 121);
    assert(!ParseWeatherLocation("{}"));
    assert(!ParseWeatherLocation("{\"results\":[{\"latitude\":31,\"longitude\":121}],\"extra\":" +
                                 std::string(17, '[') + "0" + std::string(17, ']') + "}"));
    assert(!ParseWeatherLocation(R"({"results":[{"latitude":91,"longitude":121}]})"));
    assert(!ParseWeatherLocation(R"({"results":[{"latitude":"31","longitude":121}]})"));
    auto weather = ParseCurrentWeather(
        "上海",
        R"({"current":{"temperature_2m":24.8,"relative_humidity_2m":55,"weather_code":2,"time":"2026-10-01T14:15"}})");
    assert(weather && weather->temperature_c == 25 && weather->humidity_percent == 55);
    assert(weather->condition == "多云" && weather->updated_at == "2026-10-01T14:15");
    assert(!ParseCurrentWeather("上海", "{}"));
    assert(!ParseCurrentWeather(
        "上海",
        std::string(4097, ' ') +
            R"({"current":{"temperature_2m":24.8,"relative_humidity_2m":55,"weather_code":2,"time":"2026-10-01T14:15"}})"));
    assert(!ParseCurrentWeather(
        "上海",
        R"({"current":{"temperature_2m":null,"relative_humidity_2m":55,"weather_code":2,"time":"2026-10-01T14:15"}})"));
    assert(!ParseCurrentWeather(
        "上海",
        R"({"current":{"temperature_2m":20,"relative_humidity_2m":101,"weather_code":2,"time":"2026-10-01T14:15"}})"));
    assert(!ParseCurrentWeather(
        "上海",
        R"({"current":{"temperature_2m":20,"relative_humidity_2m":55,"weather_code":1000,"time":"2026-10-01T14:15"}})"));
    assert(WeatherCondition(0) == "晴" && WeatherCondition(95) == "雷雨");
    assert(EncodeWeatherCity("A B&上海") == "A%20B%26%E4%B8%8A%E6%B5%B7");
}
