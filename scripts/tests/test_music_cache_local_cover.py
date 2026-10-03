"""Exercise cache artwork identity for managed local audio on real files."""

import os
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
MUSIC = ROOT / "main/music"


class MusicCacheLocalCoverTests(unittest.TestCase):
    def test_managed_local_artwork_preserves_original_identity_and_user_files(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = pathlib.Path(directory)
            executable = folder / "cache-local-cover"
            result = subprocess.run([
                os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-I" + str(MUSIC), str(ROOT / "scripts/tests/cpp/music_cache_local_cover_test.cc"),
                str(MUSIC / "music_cache.cc"), str(MUSIC / "music_util.cc"),
                "-o", str(executable),
            ], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable), str(folder / "sdcard")],
                                    capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
