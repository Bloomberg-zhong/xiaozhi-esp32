"""Exercise the board's real percentage UI methods with LVGL boundary stubs."""
import os
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
BOARD = ROOT / 'main/boards/waveshare/esp32-s3-rlcd-4.2'


class RlcdBatteryUiTests(unittest.TestCase):
    def test_percentage_shares_icon_row_and_updates_without_word_prefix(self):
        source = (BOARD / 'dashboard_ui.cc').read_text()
        self.assertIn('void CustomLcdDisplay::SetupBatteryPercentageUI()', source)
        start = source.index('void SetText(')
        set_text = source[start:source.index('\nvoid StylePage(', start)]
        start = source.index('void CustomLcdDisplay::SetupBatteryPercentageUI()')
        methods = source[start:source.index('void CustomLcdDisplay::SetupDashboardUI()', start)]
        driver = (ROOT / 'scripts/tests/cpp/rlcd_battery_ui_test.cc').read_text()
        driver = driver.replace('// PRODUCTION_METHODS', set_text + '\n' + methods)
        with tempfile.TemporaryDirectory() as folder:
            path = pathlib.Path(folder) / 'battery.cc'
            path.write_text(driver)
            exe = pathlib.Path(folder) / 'battery'
            result = subprocess.run([
                os.environ.get('CXX', 'clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                str(path), '-o', str(exe),
            ], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
