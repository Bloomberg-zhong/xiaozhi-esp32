import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
BOARD_DIR = ROOT / "main" / "boards" / "waveshare" / "esp32-s3-rlcd-4.2"


class RlcdMusicUiTests(unittest.TestCase):
    def test_play_pause_chat_stop_and_progress(self):
        compiler = next(
            (name for name in (os.environ.get("CXX"), "clang++", "g++")
             if name and shutil.which(name)), None,
        )
        if compiler is None:
            self.skipTest("no host C++ compiler found")
        with tempfile.TemporaryDirectory() as folder:
            executable = pathlib.Path(folder) / "music_ui_test"
            result = subprocess.run(
                [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 f"-I{BOARD_DIR}", f"-I{ROOT / 'main'}",
                 str(ROOT / "scripts/tests/cpp/rlcd_music_ui_test.cc"),
                 "-o", str(executable)], capture_output=True, text=True,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
