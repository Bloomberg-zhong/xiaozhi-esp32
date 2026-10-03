"""Run production request selection against real SD files and cache metadata."""

import os
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
MUSIC = ROOT / "main/music"


class MusicLocalPreferenceTests(unittest.TestCase):
    def test_implicit_song_query_prefers_stored_tracks_and_preserves_source_selection(self):
        source = (MUSIC / "music_tools.cc").read_text()
        selection = "struct SearchOutcome {" + source.split("struct SearchOutcome {", 1)[1].split(
            "bool ParseModeArgument(", 1
        )[0]
        driver = (ROOT / "scripts/tests/cpp/music_local_preference_test.cc").read_text()
        with tempfile.TemporaryDirectory() as directory:
            folder = pathlib.Path(directory)
            path = folder / "selection.cc"
            path.write_text(driver.replace("// PRODUCTION_SELECTION", selection))
            executable = folder / "selection"
            result = subprocess.run([
                os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-I" + str(MUSIC), str(path), str(MUSIC / "local_music.cc"),
                str(MUSIC / "music_cache.cc"), str(MUSIC / "music_util.cc"),
                "-o", str(executable),
            ], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable), str(folder / "sdcard")],
                                    capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
