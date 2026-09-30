import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
BOARD_DIR = ROOT / "main" / "boards" / "waveshare" / "esp32-s3-rlcd-4.2"
CPP_TEST = ROOT / "scripts" / "tests" / "cpp" / "rlcd_pixel_map_test.cc"


class RlcdPixelMapTests(unittest.TestCase):
    def test_matches_the_lookup_tables_it_replaced(self):
        compiler = next(
            (name for name in (os.environ.get("CXX"), "clang++", "g++") if name and shutil.which(name)),
            None,
        )
        if compiler is None:
            self.skipTest("no host C++ compiler found")
        with tempfile.TemporaryDirectory() as temp_dir:
            executable = pathlib.Path(temp_dir) / "rlcd_pixel_map_test"
            compiled = subprocess.run(
                [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", f"-I{BOARD_DIR}",
                 str(CPP_TEST), "-o", str(executable)],
                capture_output=True, text=True, check=False,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, check=False)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
