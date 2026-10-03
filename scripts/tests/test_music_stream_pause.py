"""Run the real reader/writer against a Range-capable in-memory HTTP stream."""

import os
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


class MusicStreamPauseTest(unittest.TestCase):
    def test_pause_releases_http_and_resumes_without_losing_partial_chunk(self):
        source = (ROOT / "main/music/music_player.cc").read_text()
        writer_start = source.rfind("\n", 0, source.index("MusicPlayer::WriteToBuffer(")) + 1
        writer = source[writer_start:source.index("void MusicPlayer::LoadLyrics(")]
        declaration = writer.split("{", 1)[0].replace("MusicPlayer::", "").strip() + ";"
        reader = "void MusicPlayer::NetTask(" + source.split("void MusicPlayer::NetTask(", 1)[1].split(
            "bool MusicPlayer::CanDownloadCache(", 1
        )[0]
        driver = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "music_source.h"
#include "music_cache.h"
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define TAG "test"
#define pdMS_TO_TICKS(x) (x)
constexpr size_t kNetChunkSize = 2048;
constexpr int kMaxStreamRetries = 3;
constexpr int kStreamTimeoutMs = 10000, kReadPollMs = 1000;
enum class NetworkErrc { Timeout };
struct NetworkError { NetworkErrc code = NetworkErrc::Timeout; };
struct ReadResult : std::optional<int> {
    using std::optional<int>::optional;
    NetworkError error() { return {}; }
};
struct Buffer { std::string bytes; };
struct Session {
    Buffer storage;
    Buffer* buffer = &storage;
    MusicTrack track;
    std::string local_root;
    std::shared_ptr<MusicSource> source;
    std::shared_ptr<MusicCache::Entry> cache_entry;
    std::atomic<bool> paused{false}, cancelled{false}, net_done{false}, net_failed{false};
    std::mutex mutex;
    std::string error;
};
static Session* active = nullptr;
static int sends = 0, closes = 0;
static std::vector<size_t> opens;
static int mode = 0;
static bool timed_out = false;
size_t xStreamBufferSend(Buffer* buffer, const char* data, size_t size, int) {
    if (active->paused) {
        if (++sends > 10) active->cancelled = true; // fail the old blocked writer, don't hang host
        return 0;
    }
    const size_t written = std::min<size_t>(size, 2);
    buffer->bytes.append(data, written);
    if (++sends == 1 && mode == 0) active->paused = true; // pause halfway through a chunk
    return written;
}
void vTaskDelay(int) {
    assert(closes >= 1); // the paused reader must close before waiting for resume
    active->paused = false;
}
bool IsLocalMusicPath(const std::string&) { return false; }
class Http {
public:
    size_t offset;
    explicit Http(size_t start) : offset(start) {}
    void SetTimeout(int timeout) { assert(timeout > 0 && timeout <= 1000); }
    ReadResult Read(char* data, size_t capacity) {
        if (mode != 0 && !timed_out) {
            timed_out = true;
            if (mode == 1) active->paused = true; // pause while waiting for HTTP data
            return std::nullopt;
        }
        const std::string song = "abcdefgh";
        size_t size = std::min({capacity, size_t(4), song.size() - offset});
        std::memcpy(data, song.data() + offset, size);
        offset += size;
        return static_cast<int>(size);
    }
    void Close() { ++closes; }
};
class MusicPlayer {
public:
    using Session = ::Session;
    struct CacheJob { MusicTrack track; std::shared_ptr<MusicSource> source; std::string root; };
    void QueueCacheJob(CacheJob) {}
    bool IsActiveCacheJob(const MusicTrack&, const std::string&, const std::string&) { return false; }
    void NetTask(const std::shared_ptr<Session>& session);
    void ReadLocalFile(const std::shared_ptr<Session>&) { assert(false); }
    void LoadLyrics(const std::shared_ptr<Session>&) {}
    std::unique_ptr<Http> OpenStream(Session&, size_t offset, size_t& total) {
        opens.push_back(offset);
        total = 8;
        return std::make_unique<Http>(offset);
    }
'''
        driver += declaration + "\n};\n" + writer + reader
        driver += r'''
int main() {
    MusicPlayer player;
    auto session = std::make_shared<Session>();
    active = session.get();
    player.NetTask(session);
    assert(session->storage.bytes == "abcdefgh");
    assert((opens == std::vector<size_t>{0, 2}));
    assert(closes == 2);
    assert(session->net_done && !session->net_failed && !session->cancelled);
    for (mode = 1; mode <= 2; ++mode) {
        sends = closes = 0;
        timed_out = false;
        opens.clear();
        session = std::make_shared<Session>();
        active = session.get();
        player.NetTask(session);
        assert(session->storage.bytes == "abcdefgh");
        assert(session->net_done && !session->net_failed && !session->cancelled);
        if (mode == 1) {
            assert((opens == std::vector<size_t>{0, 0}));
            assert(closes == 2);
        } else {
            assert((opens == std::vector<size_t>{0})); // transient timeout keeps the connection
            assert(closes == 1);
        }
    }
}
'''
        with tempfile.TemporaryDirectory() as temp:
            cpp = pathlib.Path(temp) / "pause.cc"
            exe = pathlib.Path(temp) / "pause"
            cpp.write_text(driver)
            result = subprocess.run(
                [os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 "-I" + str(ROOT / "main/music"), str(cpp), str(ROOT / "main/music/music_cache.cc"),
                 str(ROOT / "main/music/music_util.cc"), "-o", str(exe)],
                capture_output=True, text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
