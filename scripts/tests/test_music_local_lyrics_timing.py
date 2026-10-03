"""Local playback loads real LRC before the audio buffer can apply backpressure."""
import os
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


class MusicLocalLyricsTimingTests(unittest.TestCase):
    def test_local_lyrics_are_ready_before_first_audio_write(self):
        source = (ROOT / "main/music/music_player.cc").read_text()
        methods = "void MusicPlayer::LoadLyrics(" + source.split(
            "void MusicPlayer::LoadLyrics(", 1)[1].split(
            "void MusicPlayer::NetTask(", 1)[0]
        driver = r'''
#include "music_cache.h"
#include "local_music.h"
#include "lrc_parser.h"
#include <atomic>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unistd.h>
#define TAG "test"
void Log(const char*, const char*, ...) {}
#define ESP_LOGI(...) Log(__VA_ARGS__)
#define pdMS_TO_TICKS(x) (x)
void vTaskDelay(int) {}
constexpr size_t kMaxLocalLyricBytes = 65536, kNetChunkSize = 2048;
struct Session {
    MusicTrack track;
    std::shared_ptr<MusicSource> source;
    std::string local_root, error;
    std::mutex mutex;
    std::atomic<bool> cancelled{false}, net_failed{false}, net_done{false}, local_stream{false};
};
class MusicPlayer {
public:
    using Session = ::Session;
    std::mutex mutex_;
    std::shared_ptr<Session> session_;
    std::vector<LyricLine> lyrics_;
    int lyric_index_ = 0;
    std::string audio;
    void LoadLyrics(const std::shared_ptr<Session>&);
    void ReadLocalFile(const std::shared_ptr<Session>&);
    size_t WriteToBuffer(Session&, const char* data, size_t size) {
        // Audio backpressure may stall here for the entire remaining song.
        // Assert the real LoadLyrics+parser have already made lyrics visible.
        assert(lyrics_.size() == 1 && lyrics_[0].time_ms == 1234);
        assert(lyrics_[0].text == "ready at playback start");
        audio.append(data, size);
        return size;
    }
};
'''
        driver += methods
        driver += r'''
int main() {
    char temp[] = "/tmp/xz-local-lyric-timing-XXXXXX";
    const std::string root = mkdtemp(temp);
    const std::string bytes = "ID3" + std::string(5000, 'a');
    std::ofstream(root + "/song.mp3", std::ios::binary) << bytes;
    std::ofstream(root + "/song.lrc") << "[00:01.234]ready at playback start\n";
    MusicPlayer player;
    auto session = std::make_shared<Session>();
    session->track.stream_url = root + "/song.mp3";
    session->track.lyric_url = root + "/song.lrc";
    session->local_root = root;
    player.session_ = session;
    player.ReadLocalFile(session);
    assert(session->net_done && !session->net_failed);
    assert(player.audio == bytes);
    std::filesystem::remove_all(root);
}
'''
        with tempfile.TemporaryDirectory() as temp:
            cpp = pathlib.Path(temp) / "local-lyrics.cc"
            cpp.write_text(driver)
            exe = pathlib.Path(temp) / "local-lyrics"
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
