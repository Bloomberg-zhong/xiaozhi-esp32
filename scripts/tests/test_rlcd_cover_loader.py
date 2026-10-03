"""Run the production worker/cache against host IDF, HTTP and JPEG stubs."""

import os
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
BOARD = ROOT / "main/boards/waveshare/esp32-s3-rlcd-4.2"
STUBS = ROOT / "scripts/tests/fixtures/rlcd_cover_loader"


class CoverLoaderTests(unittest.TestCase):
    def test_worker_lifecycle_cache_recovery_and_latest_request(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = pathlib.Path(directory)
            (folder / "freertos").mkdir()
            (folder / "sdkconfig.h").write_text("#define CONFIG_USE_MUSIC_PLAYER 1\n")
            for name in ("esp_heap_caps.h", "esp_timer.h", "freertos/FreeRTOS.h",
                         "freertos/task.h", "application.h", "board.h", "lvgl_image.h",
                         "jpeg_to_image.h"):
                (folder / name).write_text('#pragma once\n#include "stubs.h"\n')
            (folder / "esp_log.h").write_text(
                "#pragma once\n#define ESP_LOGI(...) ((void)0)\n#define ESP_LOGW(...) ((void)0)\n")
            executable = folder / "loader_test"
            compiler = os.environ.get("CXX", "clang++")
            command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
                       "-I" + str(folder), "-I" + str(STUBS), "-I" + str(BOARD),
                       "-I" + str(ROOT / "main/music"),
                       str(ROOT / "scripts/tests/cpp/rlcd_cover_loader_test.cc"),
                       str(BOARD / "music_cover_loader.cc"), str(BOARD / "music_cover_model.cc"),
                       str(ROOT / "main/music/music_cache.cc"),
                       str(ROOT / "main/music/music_util.cc"), "-o", str(executable)]
            result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            selected = os.environ.get("RLCD_COVER_SCENARIO")
            command = [str(executable), str(folder / "sdcard")]
            if selected:
                command.append(selected)
            result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            for scenario in ("voice", "latest", "corrupt", "destroy_io", "destroy_queued",
                             "invalid", "oversized", "deadline_redirect", "local_missing",
                             "local_corrupt", "local_user", "retry", "retry_bounded",
                             "retry_cancel"):
                if not selected or scenario == selected:
                    self.assertIn(scenario + " passed", result.stdout)


if __name__ == "__main__":
    unittest.main()
