"""Run actual cache publication with FAT-like no-overwrite rename semantics."""

import os
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
MUSIC = ROOT / "main/music"


class MusicCacheFatTests(unittest.TestCase):
    def test_owned_replacements_and_failed_publication_keep_user_files_and_resumable_bytes(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = pathlib.Path(directory)
            header = folder / "fat_rename.h"
            header.write_text('''#pragma once
#include <cstdio>
namespace fat_test { int Rename(const char*, const char*); }
namespace std {
inline int FatRename(const char* from, const char* to) { return fat_test::Rename(from, to); }
}
#define rename FatRename
''')
            obj = folder / "cache.o"
            executable = folder / "fat"
            compiler = os.environ.get("CXX", "c++")
            commands = [
                [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I" + str(MUSIC),
                 "-include", str(header), "-c", str(MUSIC / "music_cache.cc"), "-o", str(obj)],
                [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I" + str(MUSIC),
                 str(ROOT / "scripts/tests/cpp/music_cache_fat_test.cc"), str(obj),
                 str(MUSIC / "music_util.cc"), "-o", str(executable)],
                [str(executable), str(folder / "sdcard")],
            ]
            for command in commands:
                result = subprocess.run(command, capture_output=True, text=True, timeout=20)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
