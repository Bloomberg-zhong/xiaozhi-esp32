"""Exercise pending-download pressure eviction and cover reservations on real files."""

import os
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
MUSIC = ROOT / "main/music"


class MusicCacheGcTests(unittest.TestCase):
    def test_abandoned_owned_partials_release_pressure_and_cover_counts_audio_reservations(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = pathlib.Path(directory)
            executable = folder / "cache-gc"
            result = subprocess.run([
                os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-I" + str(MUSIC), str(ROOT / "scripts/tests/cpp/music_cache_gc_test.cc"),
                str(MUSIC / "music_cache.cc"), str(MUSIC / "music_util.cc"),
                "-o", str(executable),
            ], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable), str(folder / "sdcard")],
                                    capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
