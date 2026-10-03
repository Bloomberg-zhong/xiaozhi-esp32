"""Run desktop page selection, reminders, and sensor decoding on the host."""
import os
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
BOARD = ROOT / 'main/boards/waveshare/esp32-s3-rlcd-4.2'

class RlcdDashboardLogicTests(unittest.TestCase):
    def test_desktop_pages_reminders_and_sensor_data(self):
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
