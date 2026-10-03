"""Run desktop page selection, calendar, reminders, and sensor decoding on the host."""
import os
import hashlib
import json
import re
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
BOARD = ROOT / 'main/boards/waveshare/esp32-s3-rlcd-4.2'

class RlcdDashboardLogicTests(unittest.TestCase):
    def test_calendar_font_covers_offline_chinese_and_fits_six_rows(self):
        font = (BOARD / 'dashboard_calendar_font.c').read_text()
        glyphs = {chr(int(value, 16)) for value in re.findall(r'/\* U\+([0-9A-F]+)', font)}
        ui = (BOARD / 'dashboard_ui.cc').read_text()
        model = (BOARD / 'dashboard_model.cc').read_text()
        needed = {char for char in ui + model if '\u4e00' <= char <= '\u9fff'}
        self.assertFalse(needed - glyphs, 'Missing offline calendar glyphs: ' + ''.join(sorted(needed - glyphs)))
        line_height = int(re.search(r'\.line_height\s*=\s*(\d+)', font).group(1))
        # Two lines per 31px label; six rows plus header/footer fit the 264px content area.
        self.assertLessEqual(line_height * 2 - 3, 31)
        self.assertLessEqual(49 + 5 * 32 + 31, 241)
        self.assertLessEqual(246 + line_height, 264)

    def test_every_lunar_date_matches_hko_1901_through_2100(self):
        expected = json.loads((ROOT / 'scripts/tests/fixtures/rlcd_lunar_hko.json').read_text())
        with tempfile.TemporaryDirectory() as folder:
            exe = pathlib.Path(folder) / 'lunar'
            result = subprocess.run([
                os.environ.get('CXX', 'clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-I' + str(BOARD), '-I' + str(ROOT / 'main'),
                str(ROOT / 'scripts/tests/cpp/rlcd_lunar_all_dates_test.cc'),
                str(BOARD / 'dashboard_model.cc'), '-o', str(exe),
            ], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(len(result.stdout.splitlines()), expected['days'])
            self.assertEqual(hashlib.sha256(result.stdout).hexdigest(), expected['sha256'])

    def test_desktop_pages_calendar_reminders_and_sensor_data(self):
        with tempfile.TemporaryDirectory() as folder:
            exe = pathlib.Path(folder) / 'dashboard'
            result = subprocess.run([
                os.environ.get('CXX', 'clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-I' + str(BOARD), '-I' + str(ROOT / 'main'),
                str(ROOT / 'scripts/tests/cpp/rlcd_dashboard_logic_test.cc'),
                str(BOARD / 'dashboard_model.cc'), '-o', str(exe),
            ], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
