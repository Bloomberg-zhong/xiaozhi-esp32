"""USB power icon uses a detected host link, without inventing charger current."""
import os
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
BOARD = ROOT / 'main/boards/waveshare/esp32-s3-rlcd-4.2'


class UsbPowerTests(unittest.TestCase):
    def test_usb_connection_shows_bolt_and_disconnect_restores_percentage_icon(self):
        source = (ROOT / 'main/display/lvgl_display/lvgl_display.cc').read_text()
        start = source.index('        if (charging) {') if '        if (charging) {' in source else source.index('        if (charging ||')
        end = source.index('        DisplayLockGuard lock(this);', start)
        selection = source[start:end]
        driver = '''
#include <cassert>
#include <cstring>
#define MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_BOLT "bolt"
#define MATERIAL_SYMBOLS_BATTERY_ANDROID_0 "0"
#define MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_1 "1"
#define MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_2 "2"
#define MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_3 "3"
#define MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_4 "4"
#define MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_5 "5"
#define MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_6 "6"
#define MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_FULL "full"
bool host = false;
bool ShouldShowUsbPowerIcon() { return host; }
const char* Select(int battery_level, bool charging) {
 const char* icon = nullptr;
// PRODUCTION
 return icon;
}
int main() {
 assert(!strcmp(Select(98, false), "6"));
 host = true;
 assert(!strcmp(Select(98, false), "bolt"));
 assert(!strcmp(Select(100, false), "bolt")); // USB is power, not measured charge current
 host = false;
 assert(!strcmp(Select(100, false), "full"));
 assert(!strcmp(Select(20, true), "bolt")); // boards with real charger status retain it
}
'''.replace('// PRODUCTION', selection)
        with tempfile.TemporaryDirectory() as folder:
            path = pathlib.Path(folder) / 'power.cc'
            path.write_text(driver)
            result = subprocess.run([os.environ.get('CXX', 'clang++'), '-std=c++17',
                                     str(path), '-o', str(path.with_suffix(''))], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(path.with_suffix(''))], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
