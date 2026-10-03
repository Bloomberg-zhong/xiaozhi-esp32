"""Run production LoadLyrics with the real SD cache, parser, and local reader."""
import os
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


class MusicCachedLyricsTest(unittest.TestCase):
    def test_companion_ownership_bounds_budget_and_corruption(self):
        with tempfile.TemporaryDirectory() as temp:
            exe = pathlib.Path(temp) / "lyric-boundaries"
            result = subprocess.run([
                os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-I" + str(ROOT / "main/music"),
                str(ROOT / "scripts/tests/cpp/music_cached_lyrics_test.cc"),
                str(ROOT / "main/music/music_cache.cc"),
                str(ROOT / "main/music/music_util.cc"), "-o", str(exe)],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_downloaded_lyrics_survive_writer_snapshot_and_offline_playback(self):
        source = (ROOT / "main/music/music_player.cc").read_text()
        load = "void MusicPlayer::LoadLyrics(" + source.split(
            "void MusicPlayer::LoadLyrics(", 1)[1].split(
            "void MusicPlayer::ReadLocalFile(", 1)[0]
        driver = r'''
#include "music_cache.h"
#include "local_music.h"
#include "lrc_parser.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unistd.h>
#define TAG "test"
void Log(const char*, const char*, ...) {}
#define ESP_LOGI(...) Log(__VA_ARGS__)
constexpr size_t kMaxLocalLyricBytes = 64 * 1024;
class Source : public MusicSource {
    const std::string base_ = "https://source";
public:
    int downloads = 0;
    std::vector<LyricLine> response{{1001, "first"}, {61234, "[chorus] second"}};
    const char* type() const override { return "http"; }
    const std::string& base_url() const override { return base_; }
    bool Search(const std::string&, const std::string&, int,
                std::vector<MusicTrack>&, std::string&) override { return false; }
    MusicTrack BuildTrack(const std::string&, const std::string&,
                          const std::string&, bool) const override { return {}; }
    bool Ping(std::string&) override { return false; }
    std::vector<LyricLine> FetchLyrics(const MusicTrack&) override {
        ++downloads;
        return response;
    }
};
std::vector<LyricLine> MusicSource::FetchLyrics(const MusicTrack&) { return {}; }
struct Session {
    MusicTrack track;
    std::shared_ptr<MusicSource> source;
    std::string local_root;
};
class MusicPlayer {
public:
    using Session = ::Session;
    std::mutex mutex_;
    std::shared_ptr<Session> session_;
    std::vector<LyricLine> lyrics_;
    int lyric_index_ = 0;
    void LoadLyrics(const std::shared_ptr<Session>&);
};
'''
        driver += load
        driver += r'''
int main() {
    char temp[] = "/tmp/xz-cache-lyrics-XXXXXX";
    const std::string root = mkdtemp(temp);
    MusicCache cache(root, {1024 * 1024, 0});
    auto source = std::make_shared<Source>();
    MusicPlayer player;
    auto session = std::make_shared<Session>();
    session->source = source;
    session->local_root = root;
    session->track.id = "song";
    session->track.title = "Song";
    session->track.artist = "Artist";
    session->track.stream_url = "https://source/stream?secret=hidden";
    player.session_ = session;
    auto writer = cache.Begin(session->track, source->base_url(), 8);
    assert(writer);
    player.LoadLyrics(session);
    assert(player.lyrics_.size() == 2 && player.lyrics_[1].time_ms == 61234);
    assert(writer->Append(0, "ID3abcde", 8) && writer->Finish());
    auto hit = cache.Find(session->track, source->base_url());
    assert(hit && !hit->track.lyric_url.empty());
    std::string text;
    assert(ReadLocalTextFile(hit->track.lyric_url, 65536, text));
    assert(text == "[00:01.001] first\n[01:01.234] [chorus] second\n");
    auto offline = std::make_shared<Session>();
    offline->track = hit->track;
    offline->local_root = root;
    player.session_ = offline;
    player.lyrics_.clear();
    player.LoadLyrics(offline);
    assert(player.lyrics_.size() == 2 && player.lyrics_[0].time_ms == 1001);
    assert(player.lyrics_[1].text == "[chorus] second");
    assert(source->downloads == 1);
    player.session_ = session;
    player.LoadLyrics(session);
    assert(source->downloads == 1);  // Remote repeat reuses the saved lyrics too.
    std::filesystem::remove_all(root);
}
'''
        with tempfile.TemporaryDirectory() as temp:
            cpp = pathlib.Path(temp) / "cached-lyrics.cc"
            cpp.write_text(driver)
            exe = pathlib.Path(temp) / "cached-lyrics"
            result = subprocess.run([
                os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-I" + str(ROOT / "main/music"), str(cpp),
                str(ROOT / "main/music/music_cache.cc"),
                str(ROOT / "main/music/local_music.cc"),
                str(ROOT / "main/music/music_util.cc"),
                str(ROOT / "main/music/lrc_parser.cc"), "-o", str(exe)],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
