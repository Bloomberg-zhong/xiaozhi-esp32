"""Board clock policy preserves microphone/wake and full active performance."""
import os
import pathlib
import subprocess
import tempfile
import unittest
from test_rlcd_offline_music import method
ROOT = pathlib.Path(__file__).resolve().parents[2]
BOARD = ROOT / 'main/boards/waveshare/esp32-s3-rlcd-4.2/waveshare-s3-rlcd-4.2.cc'

class RlcdCpuPowerTests(unittest.TestCase):
    def test_idle_dfs_and_active_full_speed_with_pm_on_and_off(self):
        source = BOARD.read_text()
        self.assertIn('void ConfigureCpuPower(', source)
        self.assertIn('ConfigureCpuPower(level);', method(source, 'virtual void SetPowerSaveLevel('))
        self.assertIn('new PowerSaveTimer(-1,', source) # microphone must stay enabled
        production = method(source, 'void ConfigureCpuPower(')
        driver = r'''
#include <cassert>
#define CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ 240
#define ESP_OK 0
#define TAG "test"
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
enum class PowerSaveLevel { LOW_POWER, BALANCED, PERFORMANCE };
struct esp_pm_config_t { int max_freq_mhz; int min_freq_mhz; bool light_sleep_enable; };
static esp_pm_config_t last{};
static int calls = 0;
int esp_pm_configure(esp_pm_config_t* config) { last = *config; ++calls; return 0; }
struct Board {
// PRODUCTION
};
int main() {
    Board board;
    board.ConfigureCpuPower(PowerSaveLevel::LOW_POWER);
#if CONFIG_PM_ENABLE
    assert(calls == 1 && last.min_freq_mhz == 80 && last.max_freq_mhz == 240);
    assert(!last.light_sleep_enable);
    board.ConfigureCpuPower(PowerSaveLevel::BALANCED);
    assert(last.min_freq_mhz == 80 && !last.light_sleep_enable);
    board.ConfigureCpuPower(PowerSaveLevel::PERFORMANCE);
    assert(last.min_freq_mhz == 240 && last.max_freq_mhz == 240);
    assert(!last.light_sleep_enable);
#else
    assert(calls == 0);
#endif
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / 'test.cc'
            path.write_text(driver.replace('// PRODUCTION', production))
            for enabled in (0,1):
                exe = pathlib.Path(tmp) / f'test{enabled}'
                result = subprocess.run([os.environ.get('CXX','clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-unused-variable', f'-DCONFIG_PM_ENABLE={enabled}', str(path), '-o', str(exe)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                result = subprocess.run([str(exe)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
