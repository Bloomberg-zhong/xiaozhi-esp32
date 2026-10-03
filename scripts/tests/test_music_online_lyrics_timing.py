"""Slow optional lyrics must finish before the audible HTTP stream starts."""

import os
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


class MusicOnlineLyricsTimingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = (ROOT / "main/music/music_player.cc").read_text()
        methods = "void MusicPlayer::LoadLyrics(" + source.split(
            "void MusicPlayer::LoadLyrics(", 1
        )[1].split("bool MusicPlayer::CanDownloadCache(", 1)[0]
        driver = r'''
#include "music_cache.h"
#include "local_music.h"
#include "lrc_parser.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unistd.h>
#define TAG "test"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define pdMS_TO_TICKS(x) (x)
constexpr size_t kNetChunkSize = 2048, kMaxLocalLyricBytes = 65536;
[[maybe_unused]] constexpr size_t kLyricsAfterBytes = 32768;
constexpr int kMaxStreamRetries = 3, kStreamTimeoutMs = 10000, kReadPollMs = 1000;
enum class NetworkErrc { Timeout };
struct NetworkError { NetworkErrc code = NetworkErrc::Timeout; };
struct ReadResult : std::optional<int> {
    using std::optional<int>::optional;
    NetworkError error() const { return {}; }
};
struct Session {
    MusicTrack track;
    std::string local_root, error;
    std::shared_ptr<MusicSource> source;
    std::shared_ptr<MusicCache::Entry> cache_entry;
    std::atomic<bool> paused{false}, cancelled{false}, net_done{false}, net_failed{false};
    std::atomic<bool> local_stream{false};
    std::mutex mutex;
};
static Session* active = nullptr;
static std::string scenario, audio;
static int virtual_ms = 0, lyric_calls = 0, opens = 0;
static int first_open_ms = -1, first_audio_ms = -1;
static const std::string song = "ID3" + std::string(40000, 'a');
void vTaskDelay(int ms) { virtual_ms += ms; }
struct Source : MusicSource {
    std::string base = "https://music.example";
    const char* type() const override { return "http"; }
    const std::string& base_url() const override { return base; }
    bool Search(const std::string&, const std::string&, int,
                std::vector<MusicTrack>&, std::string&) override { return false; }
    MusicTrack BuildTrack(const std::string&, const std::string&,
                          const std::string&, bool) const override { return {}; }
    bool Ping(std::string&) override { return true; }
    std::vector<LyricLine> FetchLyrics(const MusicTrack&) override {
        ++lyric_calls;
        // Model a slow request without sleeping. If this starts after audio or
        // body HTTP, it steals the only producer for eight playback seconds.
        assert(opens == 0 && first_audio_ms < 0);
        virtual_ms += 8000;
        if (scenario == "cancel") active->cancelled = true;
        if (scenario == "missing" || scenario == "cancel") return {};
        return ParseLrc("[00:01.00]lyrics ready\n");
    }
};
std::vector<LyricLine> MusicSource::FetchLyrics(const MusicTrack&) { return {}; }
class Http {
public:
    size_t offset;
    explicit Http(size_t start) : offset(start) {}
    void SetTimeout(int timeout) { assert(timeout > 0); }
    ReadResult Read(char* data, size_t capacity) {
        if (offset == song.size() && scenario == "live") {
            active->cancelled = true; // endless stream is stopped by the user
            return 0;
        }
        const size_t size = std::min(capacity, song.size() - offset);
        std::memcpy(data, song.data() + offset, size);
        offset += size;
        return static_cast<int>(size);
    }
    void Close() {}
};
class MusicPlayer {
public:
    using Session = ::Session;
    struct CacheJob { MusicTrack track; std::shared_ptr<MusicSource> source; std::string root; };
    std::mutex mutex_;
    std::shared_ptr<Session> session_;
    std::vector<LyricLine> lyrics_;
    int lyric_index_ = -1;
    void LoadLyrics(const std::shared_ptr<Session>&);
    void ReadLocalFile(const std::shared_ptr<Session>&);
    void NetTask(const std::shared_ptr<Session>&);
    void QueueCacheJob(CacheJob) {}
    bool IsActiveCacheJob(const MusicTrack&, const std::string&, const std::string&) {
        return false;
    }
    size_t WriteToBuffer(Session&, const char* bytes, size_t size) {
        if (first_audio_ms < 0) first_audio_ms = virtual_ms;
        audio.append(bytes, size);
        return size;
    }
    std::unique_ptr<Http> OpenStream(Session&, size_t offset, size_t& total) {
        ++opens;
        if (first_open_ms < 0) first_open_ms = virtual_ms;
        total = scenario == "live" ? 0 : song.size();
        return std::make_unique<Http>(offset);
    }
};
'''
        driver += methods
        driver += r'''
int main(int argc, char** argv) {
    assert(argc == 2);
    scenario = argv[1];
    MusicPlayer player;
    auto source = std::make_shared<Source>();
    auto session = std::make_shared<Session>();
    active = session.get();
    player.session_ = session;
    session->source = source;
    session->track.id = "catalog:song";
    session->track.provider = "catalog";
    session->track.title = "song";
    session->track.stream_url = "https://music.example/song.mp3";
    session->track.lyric_url = "https://music.example/song.lrc";
    session->track.live = scenario == "live";
    char temporary[] = "/tmp/xz-online-lyrics-XXXXXX";
    std::string root;
    if (scenario == "cached") {
        root = mkdtemp(temporary);
        session->local_root = root;
        MusicCache cache(root);
        auto writer = cache.Begin(session->track, source->base_url(), song.size());
        assert(writer && writer->Append(0, song.data(), song.size()) && writer->Finish());
        assert(cache.StoreLyrics(session->track, source->base_url(),
                                 ParseLrc("[00:01.00]cached lyrics\n")));
    }
    player.NetTask(session);
    if (scenario == "cancel") {
        assert(lyric_calls == 1 && opens == 0 && audio.empty());
        assert(session->cancelled && !session->net_failed);
    } else if (scenario == "live") {
        assert(lyric_calls == 0 && opens == 1 && audio == song);
    } else if (scenario == "cached") {
        assert(lyric_calls == 0 && opens == 0 && audio == song);
        assert(player.lyrics_.size() == 1 && player.lyrics_[0].text == "cached lyrics");
        assert(session->net_done && !session->net_failed);
    } else {
        assert(lyric_calls == 1 && opens == 1 && audio == song);
        assert(first_open_ms == 8000 && first_audio_ms == 8000);
        assert(session->net_done && !session->net_failed);
        assert(player.lyrics_.size() == (scenario == "missing" ? 0 : 1));
    }
    session->cache_entry.reset();
    if (!root.empty()) std::filesystem::remove_all(root);
}
'''
        cls.temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temporary.cleanup)
        cpp = pathlib.Path(cls.temporary.name) / "online-lyrics.cc"
        cls.exe = pathlib.Path(cls.temporary.name) / "online-lyrics"
        cpp.write_text(driver)
        result = subprocess.run(
            [os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
             "-I" + str(ROOT / "main/music"), str(cpp),
             str(ROOT / "main/music/music_cache.cc"),
             str(ROOT / "main/music/local_music.cc"),
             str(ROOT / "main/music/music_util.cc"),
             str(ROOT / "main/music/lrc_parser.cc"), "-o", str(cls.exe)],
            capture_output=True, text=True,
        )
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_scenario(self, scenario):
        result = subprocess.run([str(self.exe), scenario], capture_output=True,
                                text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_slow_lyrics_finish_before_body_http_or_audio_write(self):
        self.run_scenario("slow")

    def test_missing_lyrics_do_not_prevent_audio(self):
        self.run_scenario("missing")

    def test_cancel_during_preparation_never_opens_body_http(self):
        self.run_scenario("cancel")

    def test_live_stream_never_fetches_lyrics(self):
        self.run_scenario("live")

    def test_complete_cache_uses_local_lyrics_without_http(self):
        self.run_scenario("cached")


if __name__ == "__main__":
    unittest.main()
