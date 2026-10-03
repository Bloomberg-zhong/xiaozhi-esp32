"""Checksum scans use bounded larger reads and still detect same-size corruption."""
import os
import pathlib
import subprocess
import tempfile
import unittest
from test_rlcd_offline_music import method

ROOT = pathlib.Path(__file__).resolve().parents[2]

class CacheReadBlocksTests(unittest.TestCase):
    def test_full_checksum_uses_at_most_one_read_per_16k(self):
        source = (ROOT / 'main/music/music_cache.cc').read_text()
        production = method(source, 'uint32_t Checksum(') + '\n' + method(source, 'bool ValidAudio(')
        driver = r'''
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>
struct Metadata { uint64_t bytes; uint32_t checksum; };
int reads = 0;
namespace std {
size_t CountRead(void* out, size_t size, size_t count, FILE* file) {
    ++reads; return std::fread(out, size, count, file);
}
}
bool RegularFile(const std::string& path, struct stat& info) {
    return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}
#define fread CountRead
// PRODUCTION
#undef fread
int main() {
    char path[] = "/tmp/xz-blocks-XXXXXX";
    int fd = mkstemp(path); assert(fd >= 0); close(fd);
    std::string bytes(1024 * 1024 + 37, 'a');
    bytes[16000] = 'b'; bytes.back() = 'c';
    FILE* file = fopen(path, "wb");
    assert(fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size()); fclose(file);
    Metadata meta{bytes.size(), Checksum(2166136261U, bytes.data(), bytes.size())};
    assert(ValidAudio(path, meta));
    assert(reads <= int((bytes.size() + 16383) / 16384 + 1));
    file = fopen(path, "r+b"); fseek(file, 900000, SEEK_SET); fputc('x', file); fclose(file);
    assert(!ValidAudio(path, meta)); // same-size damage beyond the first blocks
    unlink(path);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / 'test.cc'
            path.write_text(driver.replace('// PRODUCTION', production))
            exe = pathlib.Path(tmp) / 'test'
            result = subprocess.run([os.environ.get('CXX','clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror', str(path), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
