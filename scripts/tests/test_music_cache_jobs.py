"""Exercise production cache job arbitration without opening network sockets."""
import os
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


def method(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


class CacheJobTests(unittest.TestCase):
    def test_same_source_instances_and_active_job_never_queue_duplicate_downloads(self):
        source = (ROOT / 'main/music/music_player.cc').read_text()
        driver = r'''
#include "music_cache.h"
#include "music_util.h"
#include <atomic>
#include <cassert>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <unistd.h>
#define ESP_LOGW(...) ((void)0)
#define TAG "test"
constexpr int pdPASS = 1, kNetTaskStackSize = 8192;
int xTaskCreate(void(*)(void*), const char*, int, void*, int, void*) { return pdPASS; }
struct Source : MusicSource {
    std::string base;
    explicit Source(std::string b) : base(std::move(b)) {}
    const char* type() const override { return "http"; }
    const std::string& base_url() const override { return base; }
    bool Search(const std::string&, const std::string&, int, std::vector<MusicTrack>&, std::string&) override { return false; }
    MusicTrack BuildTrack(const std::string&, const std::string&, const std::string&, bool) const override { return {}; }
    bool Ping(std::string&) override { return true; }
    std::vector<LyricLine> FetchLyrics(const MusicTrack&) override { return {}; }
};
std::vector<LyricLine> MusicSource::FetchLyrics(const MusicTrack&) { return {}; }
enum { kDeviceStateIdle, kDeviceStatePlaying, kDeviceStateSpeaking };
struct Application {
    int state = kDeviceStateIdle;
    static Application& GetInstance() { static Application app; return app; }
    int GetDeviceState() const { return state; }
};
bool IsLocalMusicPath(const std::string& path) { return !path.empty() && path.front() == '/'; }
struct MusicPlayer {
    struct Session { bool cancelled = false, paused = false, net_done = false; MusicTrack track; std::string local_root; std::shared_ptr<MusicSource> source; };
    struct CacheJob { MusicTrack track; std::shared_ptr<MusicSource> source; std::string root; };
    std::mutex cache_jobs_mutex_;
    std::deque<CacheJob> cache_jobs_;
    std::optional<CacheJob> cache_active_job_;
    bool cache_worker_running_ = true;
    std::atomic<bool> cache_shutdown_{false};
    mutable std::mutex mutex_;
    std::shared_ptr<Session> session_;
    std::vector<MusicTrack> queue_;
    size_t index_ = 0;
    static void CacheTaskEntry(void*) {}
    void QueueCacheJob(CacheJob job);
    bool IsForegroundCacheJob(const CacheJob& job) const;
    bool IsActiveCacheJob(const MusicTrack&, const std::string&, const std::string&);
    bool CanDownloadCache() const;
    bool IsPaused() const { return session_ && session_->paused; }
};
// PRODUCTION_METHODS
int main() {
    MusicPlayer player;
    auto a = std::make_shared<Source>("https://source");
    auto b = std::make_shared<Source>("https://source");
    MusicTrack track; track.id = "song"; track.provider = "catalog";
    track.stream_url = "https://source/song?token=old";
    MusicPlayer::CacheJob job{track, a, "/sdcard"};
    player.QueueCacheJob(job);
    job.source = b; job.track.stream_url = "https://source/song?token=new";
    player.QueueCacheJob(job);
    assert(player.cache_jobs_.size() == 1);
    player.cache_jobs_.clear();
    char temporary[] = "/tmp/xz-active-cache-XXXXXX";
    job.root = mkdtemp(temporary);
    MusicCache cache(job.root);
    auto writer = cache.Begin(job.track, a->base_url(), 8);
    assert(writer && writer->Append(0, "ID", 2));
    player.cache_active_job_ = job;
    player.QueueCacheJob(job);
    assert(player.cache_jobs_.empty());
    assert(player.IsActiveCacheJob(track, a->base_url(), job.root));
    assert(!player.IsActiveCacheJob(track, "https://other-source", job.root));
    assert(writer->Suspend());
    player.QueueCacheJob(job); // retiring worker no longer owns writer: retain continuation
    assert(player.cache_jobs_.size() == 1);
    player.cache_jobs_.clear();
    job.track.provider = "other";
    player.QueueCacheJob(job);
    assert(player.cache_jobs_.size() == 1);
    job.source = std::make_shared<Source>("https://other-source");
    player.QueueCacheJob(job);
    assert(player.cache_jobs_.size() == 2);
    std::filesystem::remove_all(temporary);
    job.track.provider = "catalog"; job.source = b; job.root = "/sdcard";
    player.session_ = std::make_shared<MusicPlayer::Session>();
    player.session_->local_root = "/sdcard"; player.session_->source = a;
    player.queue_.push_back(track);
    assert(player.IsForegroundCacheJob(job));
    job.root = "/other-card";
    assert(!player.IsForegroundCacheJob(job));
    job.root = "/sdcard";
    player.session_->cancelled = true;
    assert(!player.IsForegroundCacheJob(job));
    player.session_->cancelled = false;
    Application::GetInstance().state = kDeviceStatePlaying;
    player.queue_[0].stream_url = "https://source/song";
    assert(!player.CanDownloadCache());
    player.session_->net_done = true;
    assert(player.CanDownloadCache());
    player.session_->net_done = false;
    player.queue_[0].stream_url = "/sdcard/song.mp3";
    assert(player.CanDownloadCache());
    player.session_->paused = true;
    assert(!player.CanDownloadCache());
    player.session_->paused = false;
    Application::GetInstance().state = kDeviceStateSpeaking;
    assert(!player.CanDownloadCache());
}
'''
        methods = '\n'.join(method(source, name) for name in (
            'void MusicPlayer::QueueCacheJob(', 'bool MusicPlayer::IsForegroundCacheJob(',
            'bool MusicPlayer::IsActiveCacheJob(', 'bool MusicPlayer::CanDownloadCache('))
        driver = driver.replace('// PRODUCTION_METHODS', methods)
        with tempfile.TemporaryDirectory() as directory:
            cpp, exe = pathlib.Path(directory) / 'jobs.cc', pathlib.Path(directory) / 'jobs'
            cpp.write_text(driver)
            result = subprocess.run([
                os.environ.get('CXX', 'clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-I' + str(ROOT / 'main/music'), str(cpp),
                str(ROOT / 'main/music/music_cache.cc'), str(ROOT / 'main/music/music_util.cc'),
                '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
