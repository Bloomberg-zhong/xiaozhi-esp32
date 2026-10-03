"""A verified cache pin moves once from the selected queue into its reader session."""
import os
import pathlib
import subprocess
import tempfile
import unittest
from test_rlcd_offline_music import method
ROOT = pathlib.Path(__file__).resolve().parents[2]

class PreparedEntryTests(unittest.TestCase):
    def test_pin_transfer_resume_mismatch_replacement_and_clear(self):
        source = (ROOT / 'main/music/music_player.cc').read_text()
        production = '\n'.join(method(source, signature) for signature in (
            'void MusicPlayer::SetQueue(', 'uint32_t MusicPlayer::Play(',
            'void MusicPlayer::ClearQueue(', 'bool MusicPlayer::RemoveFromQueue('))
        driver = r'''
#include <algorithm>
#include <cassert>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>
#include <string>
#include "music_cache.h"
MusicCache::Entry::~Entry() = default;
#define ESP_LOGI(...) ((void)0)
#define TAG "test"
constexpr uint32_t kSessionIdFlag = 0x80000000;
class MusicPlayer {
public:
    struct Session {
        uint32_t id = 0;
        MusicTrack track;
        std::shared_ptr<MusicSource> source;
        std::string local_root;
        std::shared_ptr<MusicCache::Entry> cache_entry;
        std::atomic<bool> cancelled{false}, paused{false};
    };
    std::mutex mutex_;
    std::vector<MusicTrack> queue_;
    size_t index_ = 0;
    bool loop_queue_ = false;
    std::string queue_tag_, local_root_;
    std::shared_ptr<MusicCache::Entry> prepared_entry_;
    std::shared_ptr<MusicSource> source_;
    std::shared_ptr<Session> session_;
    std::vector<int> lyrics_;
    int lyric_index_ = -1;
    uint32_t position_ms_ = 0, session_counter_ = 0;
    int starts = 0;
    bool StartTasks(const std::shared_ptr<Session>& session) {
        ++starts;
        assert(!session->cache_entry || session->cache_entry->track.stream_url == session->track.stream_url);
        return true;
    }
    void CheckFinished(const std::shared_ptr<Session>&) {}
    void SetQueue(std::vector<MusicTrack>, size_t, bool, std::string, std::shared_ptr<MusicCache::Entry>);
    uint32_t Play();
    void ClearQueue();
    bool RemoveFromQueue(size_t);
};
// PRODUCTION
int main() {
    MusicPlayer player;
    MusicTrack song; song.stream_url = "/sdcard/song.mp3";
    auto pin = std::make_shared<MusicCache::Entry>(); pin->track = song;
    player.SetQueue({song}, 0, false, "local", pin);
    assert(player.prepared_entry_ == pin);
    assert(player.Play() != 0);
    assert(player.session_->cache_entry == pin && !player.prepared_entry_);
    player.session_->paused = true;
    player.Play(); assert(player.starts == 1 && !player.session_->paused);
    player.session_.reset();
    MusicTrack other; other.stream_url = "/sdcard/other.mp3";
    player.SetQueue({other}, 0, false, "local", pin);
    player.Play(); assert(!player.session_->cache_entry && !player.prepared_entry_);
    player.session_.reset();
    std::weak_ptr<MusicCache::Entry> weak = pin;
    player.SetQueue({song}, 0, false, "local", std::move(pin));
    player.ClearQueue(); assert(weak.expired());
    assert(player.Play() == 0);
    auto replacement = std::make_shared<MusicCache::Entry>(); replacement->track = song;
    weak = replacement;
    player.SetQueue({song}, 0, false, "local", std::move(replacement));
    player.SetQueue({other}, 0, false, "local", nullptr);
    assert(weak.expired() && !player.prepared_entry_);
    auto removed = std::make_shared<MusicCache::Entry>(); removed->track = song;
    weak = removed;
    player.SetQueue({song}, 0, false, "local", std::move(removed));
    assert(player.RemoveFromQueue(0));
    assert(weak.expired() && !player.prepared_entry_ && player.queue_.empty());
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / 'test.cc'
            path.write_text(driver.replace('// PRODUCTION', production))
            exe = pathlib.Path(tmp) / 'test'
            result = subprocess.run([os.environ.get('CXX','clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I'+str(ROOT/'main/music'), str(path), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
