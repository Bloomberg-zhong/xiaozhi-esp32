"""Releasing a cache pin must not wait behind SD I/O owned by another worker."""
import os
import pathlib
import subprocess
import tempfile
import unittest
ROOT = pathlib.Path(__file__).resolve().parents[2]
class CachePinReleaseTests(unittest.TestCase):
    def test_release_does_not_wait_for_cache_io_mutex_and_gc_observes_lifetime(self):
        driver = r'''
#include "music_cache.cc"
#include <cassert>
#include <chrono>
#include <filesystem>
#include <future>
#include <thread>
int main(int argc, char** argv) {
    assert(argc == 2);
    std::string root = argv[1]; std::filesystem::create_directories(root);
    MusicCache cache(root, {32, 0});
    MusicTrack track; track.id = "one"; track.title = "one";
    const std::string bytes = "ID3" + std::string(17, 'a');
    auto writer = cache.Begin(track, "base", bytes.size());
    assert(writer && writer->Append(0, bytes.data(), bytes.size()) && writer->Finish());
    auto hit = cache.Find(track, "base"); assert(hit);
    auto second = cache.PinLocal(hit->track.stream_url); assert(second);
    MusicTrack other; other.id = "two"; other.title = "two";
    assert(!cache.Begin(other, "base", bytes.size())); // pin prevents eviction
    std::promise<void> acquired;
    auto ready = acquired.get_future();
    std::thread worker([&] {
        std::lock_guard<std::mutex> lock(cache_mutex);
        acquired.set_value(); std::this_thread::sleep_for(std::chrono::milliseconds(400));
    });
    ready.wait();
    auto start = std::chrono::steady_clock::now(); hit.reset();
    auto elapsed = std::chrono::steady_clock::now() - start;
    worker.join();
    assert(elapsed < std::chrono::milliseconds(100));
    assert(!cache.Begin(other, "base", bytes.size())); // remaining pin still protects
    second.reset();
    assert(cache.Begin(other, "base", bytes.size())); // last release permits eviction
    std::filesystem::remove_all(root);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / 'test.cc'; path.write_text(driver)
            exe = pathlib.Path(tmp) / 'test'
            result = subprocess.run([os.environ.get('CXX','clang++'), '-std=c++17', '-Wall','-Wextra','-Werror','-pthread', '-I'+str(ROOT/'main/music'), str(path), str(ROOT/'main/music/music_util.cc'), '-o',str(exe)], capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            result = subprocess.run([str(exe),str(pathlib.Path(tmp)/'sdcard')],capture_output=True,text=True,timeout=5)
            self.assertEqual(result.returncode,0,result.stderr)
