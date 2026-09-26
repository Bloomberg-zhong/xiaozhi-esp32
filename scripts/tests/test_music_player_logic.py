import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
MUSIC_DIR = ROOT / "main" / "music"
CPP_TEST = ROOT / "scripts" / "tests" / "cpp" / "music_player_logic_test.cc"


def find_compiler():
    for name in (os.environ.get("CXX"), "clang++", "g++"):
        if name and shutil.which(name):
            return name
    return None


class MusicPlayerLogicTests(unittest.TestCase):
    def test_pure_helpers(self):
        compiler = find_compiler()
        if compiler is None:
            self.skipTest("no host C++ compiler found")
        with tempfile.TemporaryDirectory() as temp_dir:
            executable = pathlib.Path(temp_dir) / "music_player_logic_test"
            compile_result = subprocess.run(
                [
                    compiler,
                    "-std=c++17",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"-I{MUSIC_DIR}",
                    str(CPP_TEST),
                    str(MUSIC_DIR / "lrc_parser.cc"),
                    str(MUSIC_DIR / "music_util.cc"),
                    str(MUSIC_DIR / "local_music.cc"),
                    "-o",
                    str(executable),
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(
                compile_result.returncode, 0, compile_result.stdout + compile_result.stderr
            )
            run_result = subprocess.run(
                [str(executable)], cwd=ROOT, capture_output=True, text=True, check=False
            )
            self.assertEqual(run_result.returncode, 0, run_result.stdout + run_result.stderr)


if __name__ == "__main__":
    unittest.main()
