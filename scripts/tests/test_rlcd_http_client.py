"""Production board HTTP contracts with synchronous ESP-IDF fixtures."""

import os
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
BOARD = ROOT / "main/boards/waveshare/esp32-s3-rlcd-4.2"


class RlcdHttpClientTests(unittest.TestCase):
    def test_headers_timeouts_eof_range_and_cleanup(self):
        with tempfile.TemporaryDirectory() as folder:
            executable = pathlib.Path(folder) / "http_test"
            command = [os.environ.get("CXX", "clang++"), "-std=c++17", "-Wall", "-Wextra",
                       "-Werror", "-DCONFIG_MBEDTLS_CERTIFICATE_BUNDLE=1",
                       "-I" + str(ROOT / "scripts/tests/fixtures/rlcd_http_client"),
                       "-I" + str(BOARD), str(BOARD / "rlcd_http_client.cc"),
                       str(ROOT / "scripts/tests/cpp/rlcd_http_client_test.cc"),
                       "-o", str(executable)]
            result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], cwd=ROOT, capture_output=True,
                                    text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("Synchronous HTTP contracts passed", result.stdout)


if __name__ == "__main__":
    unittest.main()
