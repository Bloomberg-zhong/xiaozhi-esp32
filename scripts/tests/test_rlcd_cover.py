"""Bounded album decoding and monochrome rendering on the RLCD."""
import os
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
BOARD = ROOT / 'main/boards/waveshare/esp32-s3-rlcd-4.2'


class CoverTests(unittest.TestCase):
    def test_dimension_validation_and_aspect_preserving_dither(self):
        with tempfile.TemporaryDirectory() as folder:
            output = pathlib.Path(folder) / 'cover-test'
            command = [os.environ.get('CXX', 'clang++'), '-std=c++17', '-I', str(BOARD),
                       str(ROOT / 'scripts/tests/cpp/rlcd_cover_test.cc'),
                       str(BOARD / 'music_cover_model.cc'), '-o', str(output)]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(output)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
