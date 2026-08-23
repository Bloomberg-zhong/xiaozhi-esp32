import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
BOARD_DIR = ROOT / "main" / "boards" / "waveshare" / "esp32-s3-rlcd-4.2"
CPP_TEST = ROOT / "scripts" / "tests" / "cpp" / "rlcd_dashboard_logic_test.cc"


class RlcdDashboardLogicTests(unittest.TestCase):
    def test_dashboard_model_behaviour(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            executable = pathlib.Path(temp_dir) / "rlcd_dashboard_logic_test"
            compile_result = subprocess.run(
                [
                    "clang++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"-I{BOARD_DIR}",
                    str(CPP_TEST),
                    str(BOARD_DIR / "dashboard_model.cc"),
                    str(BOARD_DIR / "music_gateway_model.cc"),
                    "-o",
                    str(executable),
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(
                compile_result.returncode,
                0,
                compile_result.stdout + compile_result.stderr,
            )
            run_result = subprocess.run(
                [str(executable)],
                cwd=ROOT,
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(
                run_result.returncode,
                0,
                run_result.stdout + run_result.stderr,
            )


if __name__ == "__main__":
    unittest.main()
