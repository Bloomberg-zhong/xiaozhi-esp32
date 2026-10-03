"""Compile the production weather parser against cJSON and reject bad API data."""
import os
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
BOARD = ROOT / 'main/boards/waveshare/esp32-s3-rlcd-4.2'
CJSON = ROOT / 'managed_components/espressif__cjson/cJSON'

class RlcdWeatherTests(unittest.TestCase):
    def test_ip_cache_fallback_and_changed_configuration_discard(self):
        source = (BOARD / 'dashboard_weather.cc').read_text()
        methods = source[source.index('DashboardWeather::DashboardWeather()'):source.index('bool DashboardWeather::Start()')]
        methods += source[source.index('void DashboardWeather::TaskEntry('):]
        harness = (ROOT / 'scripts/tests/cpp/rlcd_weather_task_test.cc').read_text().replace('// PRODUCTION_METHODS', methods)
        with tempfile.TemporaryDirectory() as folder:
            folder = pathlib.Path(folder)
            (folder / 'freertos').mkdir()
            (folder / 'freertos/FreeRTOS.h').write_text('#pragma once\nusing TaskHandle_t = void*;\n')
            (folder / 'freertos/task.h').write_text('#pragma once\n')
            source_path = folder / 'task.cc'
            source_path.write_text(harness)
            obj, exe = folder / 'cjson.o', folder / 'task'
            result = subprocess.run([os.environ.get('CC', 'clang'), '-c', str(CJSON / 'cJSON.c'), '-o', str(obj)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([
                os.environ.get('CXX', 'clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-I' + str(folder), '-I' + str(BOARD), '-I' + str(ROOT / 'main'), '-I' + str(CJSON),
                str(source_path), str(BOARD / 'dashboard_model.cc'), str(BOARD / 'dashboard_weather_model.cc'),
                str(obj), '-o', str(exe),
            ], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_ip_mode_migrates_old_city_and_can_be_restored(self):
        source = (BOARD / 'dashboard_weather.cc').read_text()
        config_methods = source[source.index('DashboardWeather::DashboardWeather()'):source.index('bool DashboardWeather::Start()')]
        harness = r'''
#include <cassert>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include "dashboard_weather_model.h"
std::map<std::string, std::string> values;
struct Settings {
    Settings(const char*, bool) {}
    std::string GetString(const char* key) { return values[key]; }
    bool GetBool(const char* key, bool fallback) { return values.count(key) ? values[key] == "1" : fallback; }
    void SetString(const char* key, const std::string& value) { values[key] = value; }
    void SetBool(const char* key, bool value) { values[key] = value ? "1" : "0"; }
};
namespace rlcd_dashboard {
struct DashboardStore {
    static DashboardStore& Instance() { static DashboardStore store; return store; }
    WeatherData GetWeather() { return WeatherData{"上海", "晴", 20, 50, "2026-10-01T12:00"}; }
};
}
void xTaskNotifyGive(void*) {}
class DashboardWeather {
public:
    DashboardWeather();
    std::string GetCity() const;
    bool IsAutomatic() const;
    bool Configure(const std::string&);
    mutable std::mutex mutex_;
    std::string city_;
    bool automatic_ = true;
    uint32_t revision_ = 0;
    void* task_ = nullptr;
};
'''
        assertions = r'''
int main() {
    values["wc_city"] = "上海";
    DashboardWeather fresh;
    assert(fresh.IsAutomatic() && fresh.GetCity().empty());
    assert(fresh.Configure("成都") && !fresh.IsAutomatic() && fresh.GetCity() == "成都");
    DashboardWeather manual;
    assert(!manual.IsAutomatic() && manual.GetCity() == "成都");
    auto revision = manual.revision_;
    assert(!manual.Configure("上海\n") && manual.revision_ == revision);
    assert(manual.Configure("自动") && manual.IsAutomatic() && manual.GetCity().empty());
    assert(manual.revision_ != revision);
    DashboardWeather automatic;
    assert(automatic.IsAutomatic() && automatic.GetCity().empty());
    assert(automatic.Configure("auto") && automatic.IsAutomatic());
}
'''
        with tempfile.TemporaryDirectory() as folder:
            source_path = pathlib.Path(folder) / 'config.cc'
            source_path.write_text(harness + config_methods + assertions)
            obj = pathlib.Path(folder) / 'cjson.o'
            exe = pathlib.Path(folder) / 'config'
            result = subprocess.run([os.environ.get('CC', 'clang'), '-c', str(CJSON / 'cJSON.c'), '-o', str(obj)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([
                os.environ.get('CXX', 'clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-I' + str(BOARD), '-I' + str(ROOT / 'main'), '-I' + str(CJSON),
                str(source_path), str(BOARD / 'dashboard_model.cc'),
                str(BOARD / 'dashboard_weather_model.cc'), str(obj), '-o', str(exe),
            ], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    @unittest.skipUnless((CJSON / 'cJSON.c').is_file(), 'Run ESP-IDF reconfigure to fetch the cJSON component')
    def test_current_weather_and_city_validation(self):
        with tempfile.TemporaryDirectory() as folder:
            obj = pathlib.Path(folder) / 'cjson.o'
            exe = pathlib.Path(folder) / 'weather'
            result = subprocess.run([os.environ.get('CC', 'clang'), '-c', str(CJSON / 'cJSON.c'), '-o', str(obj)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([
                os.environ.get('CXX', 'clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-I' + str(BOARD), '-I' + str(ROOT / 'main'), '-I' + str(CJSON),
                str(ROOT / 'scripts/tests/cpp/rlcd_weather_test.cc'),
                str(BOARD / 'dashboard_model.cc'), str(BOARD / 'dashboard_weather_model.cc'),
                str(obj), '-o', str(exe),
            ], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
