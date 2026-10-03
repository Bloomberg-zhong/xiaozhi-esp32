"""Exercise the production cache and recursive offline scan on real host files."""
import os
import pathlib
import subprocess
import tempfile
import unittest
ROOT = pathlib.Path(__file__).resolve().parents[2]
class MusicCacheTest(unittest.TestCase):
    def test_atomic_cache_integrity_budget_and_offline_metadata(self):
        with tempfile.TemporaryDirectory() as temp:
            exe = pathlib.Path(temp) / "cache"
            result = subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-I" + str(ROOT / "main/music"), str(ROOT / "scripts/tests/cpp/music_cache_test.cc"),
                str(ROOT / "main/music/music_cache.cc"), str(ROOT / "main/music/local_music.cc"),
                str(ROOT / "main/music/music_util.cc"), "-o", str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


class MusicCacheStreamTest(unittest.TestCase):
    def test_cancelled_reader_completes_in_single_worker_and_cache_hit_uses_no_http(self):
        source = (ROOT / "main/music/music_player.cc").read_text()
        writer_start = source.rfind("\n", 0, source.index("MusicPlayer::WriteToBuffer(")) + 1
        writer = source[writer_start:source.index("void MusicPlayer::LoadLyrics(")]
        reader = "void MusicPlayer::NetTask(" + source.split("void MusicPlayer::NetTask(", 1)[1].split(
            "bool MusicPlayer::CanDownloadCache(", 1)[0]
        worker = "void MusicPlayer::CacheTask()" + source.split("void MusicPlayer::CacheTask()", 1)[1].split(
            "bool MusicPlayer::PushFrame(", 1)[0]
        driver = r'''
#include "music_cache.h"
#include "local_music.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>
#include <unistd.h>
#define TAG "test"
void Log(const char*, const char*, ...) {}
#define ESP_LOGW(...) Log(__VA_ARGS__)
#define ESP_LOGI(...) Log(__VA_ARGS__)
#define pdMS_TO_TICKS(x) (x)
constexpr size_t kNetChunkSize = 2048;
constexpr int kMaxStreamRetries = 3, kStreamTimeoutMs = 10000, kReadPollMs = 1000;
enum class NetworkErrc { Timeout };
struct NetworkError { NetworkErrc code = NetworkErrc::Timeout; };
struct ReadResult : std::optional<int> {
 using std::optional<int>::optional; NetworkError error() { return {}; }
};
struct Buffer { std::string bytes; };
struct Session {
 Buffer storage; Buffer* buffer = &storage;
 MusicTrack track; std::shared_ptr<MusicSource> source;
 std::string local_root; std::shared_ptr<MusicCache::Entry> cache_entry;
 std::atomic<bool> paused{false}, cancelled{false}, net_done{false}, net_failed{false};
 std::mutex mutex; std::string error;
};
static Session* active = nullptr;
static bool cancel = true, interrupt_worker = false, download_allowed = true;
static bool take_over = false, foreground_matches = false;
static bool header_takeover = false;
static int opens = 0, closes = 0, remote_lyrics = 0, cache_connections = 0;
static size_t transferred = 0;
static const std::string song = "ID3abcde";
size_t xStreamBufferSend(Buffer* buffer, const char* data, size_t size, int) {
 size_t written = cancel ? std::min(size, size_t(2)) : size;
 buffer->bytes.append(data, written);
 if (cancel) active->cancelled = true;
 return written;
}
void vTaskDelay(int) { download_allowed = true; }
class Http {
 size_t offset_;
public:
 explicit Http(size_t offset) : offset_(offset) {}
 void SetTimeout(int) {}
 ReadResult Read(char* out, size_t capacity) {
  size_t size = std::min(capacity, song.size() - offset_);
  if (take_over) size = std::min(size, size_t(2));
  std::memcpy(out, song.data() + offset_, size); offset_ += size;
  transferred += size;
  if (take_over) { foreground_matches = true; take_over = false; }
  if (interrupt_worker) { download_allowed = false; interrupt_worker = false; }
  return int(size);
 }
 void Close() { ++closes; }
};
class MusicPlayer {
public:
 using Session = ::Session;
 struct CacheJob { MusicTrack track; std::shared_ptr<MusicSource> source; std::string root; };
 std::mutex cache_jobs_mutex_; std::deque<CacheJob> cache_jobs_;
 std::optional<CacheJob> cache_active_job_;
 bool cache_worker_running_ = true; std::atomic<bool> cache_shutdown_{false};
 void QueueCacheJob(CacheJob job) { cache_jobs_.push_back(std::move(job)); }
 bool CanDownloadCache() const { return download_allowed; }
 bool IsForegroundCacheJob(const CacheJob&) const { return foreground_matches; }
 bool IsActiveCacheJob(const MusicTrack&, const std::string&, const std::string&) { return false; }
 void CacheTask();
 void NetTask(const std::shared_ptr<Session>& session);
 size_t WriteToBuffer(Session& session, const char* data, size_t size);
 void LoadLyrics(const std::shared_ptr<Session>& session) {
  if (!IsLocalMusicPath(session->track.stream_url)) ++remote_lyrics;
 }
 void ReadLocalFile(const std::shared_ptr<Session>& session) {
  std::ifstream file(session->track.stream_url, std::ios::binary);
  session->storage.bytes.assign(std::istreambuf_iterator<char>(file), {});
  LoadLyrics(session); session->net_done = true;
 }
 std::unique_ptr<Http> OpenStream(Session&, size_t offset, size_t& total, int id = 4,
                                const std::function<bool()>& = {}) {
  ++opens; if (id == 8) ++cache_connections;
  if (id == 8 && header_takeover) { foreground_matches = true; header_takeover = false; }
  total = song.size(); return std::make_unique<Http>(offset);
 }
};
'''
        driver += writer + reader + worker
        driver += r'''
int main() {
 char temp[] = "/tmp/xz-cache-stream-XXXXXX";
 std::string root = mkdtemp(temp);
 MusicPlayer player;
 auto session = std::make_shared<Session>(); active = session.get();
 session->track.id = "id"; session->track.title = "title";
 session->track.stream_url = "https://source/signed?token=hidden";
 session->local_root = root;
 player.NetTask(session);
 assert(session->storage.bytes == "ID");
 assert(player.cache_jobs_.size() == 1);
 assert(ScanLocalMusic(root).empty());
 MusicCache cache(root);
 assert(cache.Pending("").size() == 1);
 cancel = false; interrupt_worker = true;
 player.CacheTask();
 assert(cache_connections >= 2 && cache.Pending("").empty());
 auto hit = cache.Find(session->track, ""); assert(hit);
 std::ifstream file(hit->track.stream_url, std::ios::binary);
 std::string bytes{std::istreambuf_iterator<char>(file), {}};
 assert(bytes == "ID3abcde");
 const int old_opens = opens, old_lyrics = remote_lyrics;
 auto cached_session = std::make_shared<Session>(); active = cached_session.get();
 cached_session->track = session->track; cached_session->local_root = root;
 player.NetTask(cached_session);
 assert(cached_session->storage.bytes == "ID3abcde");
 assert(opens == old_opens && remote_lyrics == old_lyrics);
 // A foreground replay must consume an existing prefix from SD, then request
 // only the missing suffix. Downloading the prefix again wastes the network.
 auto partial_track = session->track; partial_track.id = "partial-replay";
 auto partial = cache.Begin(partial_track, "", song.size());
 assert(partial && partial->Append(0, song.data(), 2) && partial->Suspend());
 partial.reset();
 transferred = 0;
 auto replay = std::make_shared<Session>(); active = replay.get();
 replay->track = partial_track; replay->local_root = root;
 player.NetTask(replay);
 assert(replay->storage.bytes == song && !replay->net_failed);
 assert(transferred == song.size() - 2);
 assert(cache.Find(partial_track, ""));
 // The active continuation releases its writer when the same song is selected.
 auto takeover_track = partial_track; takeover_track.id = "takeover";
 auto begun = cache.Begin(takeover_track, "", song.size());
 assert(begun && begun->Append(0, song.data(), 2) && begun->Suspend());
 begun.reset();
 player.cache_jobs_.push_back({takeover_track, nullptr, root});
 take_over = true; transferred = 0;
 player.CacheTask();
 assert(transferred == 2 && !cache.IsWriting(takeover_track, ""));
 auto jobs = cache.Pending(""); assert(jobs.size() == 1);
 auto foreground = std::make_shared<Session>(); active = foreground.get();
 foreground->track = takeover_track; foreground->local_root = root;
 transferred = 0;
 player.NetTask(foreground);
 assert(foreground->storage.bytes == song && !foreground->net_failed);
 assert(transferred == song.size() - 4 && cache.Pending("").empty());
 auto headers_track = takeover_track; headers_track.id = "header-takeover";
 auto headers_writer = cache.Begin(headers_track, "", song.size());
 assert(headers_writer && headers_writer->Append(0, song.data(), 2) && headers_writer->Suspend());
 headers_writer.reset();
 player.cache_jobs_.push_back({headers_track, nullptr, root});
 foreground_matches = false; header_takeover = true; transferred = 0;
 player.CacheTask();
 assert(transferred == 0 && !cache.IsWriting(headers_track, ""));
 auto preserved = cache.Begin(headers_track, "", song.size());
 assert(preserved && preserved->offset() == 2); // header-time takeover preserves the prefix
 preserved.reset();
 // Stop can land after the last append, before final publication. Such a
 // complete pending file must finish without asking for Range bytes=total-.
 auto full_track = headers_track; full_track.id = "full-pending";
 auto full = cache.Begin(full_track, "", song.size());
 assert(full && full->Append(0, song.data(), song.size()) && full->Suspend());
 full.reset();
 player.cache_jobs_.clear(); player.cache_jobs_.push_back({full_track, nullptr, root});
 foreground_matches = false; opens = 0; transferred = 0;
 player.CacheTask();
 assert(cache.Find(full_track, "") && opens == 1 && transferred == 0);
 std::filesystem::remove_all(root);
}
'''
        with tempfile.TemporaryDirectory() as temp:
            cpp = pathlib.Path(temp) / "cache-stream.cc"
            cpp.write_text(driver)
            exe = pathlib.Path(temp) / "cache-stream"
            result = subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-I" + str(ROOT / "main/music"), str(cpp), str(ROOT / "main/music/music_cache.cc"),
                str(ROOT / "main/music/local_music.cc"), str(ROOT / "main/music/music_util.cc"), "-o", str(exe)],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
