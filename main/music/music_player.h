#ifndef MUSIC_PLAYER_H_
#define MUSIC_PLAYER_H_

#include <freertos/FreeRTOS.h>
#include <freertos/stream_buffer.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "lrc_parser.h"
#include "music_cache.h"
#include "music_source.h"

class AudioService;
class Http;
struct cJSON;

enum class MusicPlayMode {
    kSequence,
    kRepeatAll,
    kRepeatOne,
    kShuffle,
};

const char* MusicPlayModeName(MusicPlayMode mode);
bool ParseMusicPlayMode(const std::string& name, MusicPlayMode& mode);

// Streams tracks from a MusicSource into AudioService.
//
// Each started track owns a session with two background tasks: an HTTP reader
// that fills a bounded stream buffer (in PSRAM when available) and a decoder
// that turns MP3/AAC/M4A/FLAC/WAV into 16-bit mono PCM at the codec output
// rate. Pausing retains the decoder and buffered audio, but releases the HTTP
// connection so it cannot hold Wi-Fi receive buffers needed by a conversation.
// The reader reconnects with a Range request on resume or after a network drop.
//
// Queue and status methods are thread-safe. Play(), Pause() and Stop() are
// called by Application from the main task.
class MusicPlayer {
public:
    using LyricCallback = std::function<void(uint32_t session_id, const std::string& text)>;
    using FinishedCallback =
        std::function<void(uint32_t session_id, bool success, const std::string& error)>;

    explicit MusicPlayer(AudioService& audio_service);
    ~MusicPlayer();

    void SetCallbacks(LyricCallback on_lyric, FinishedCallback on_finished);

    void SetSource(std::shared_ptr<MusicSource> source);
    std::shared_ptr<MusicSource> GetSource() const;

    // `loop` repeats the whole queue regardless of the play mode; `tag` lets
    // other features (such as the pomodoro white noise) recognize their queue.
    void SetQueue(std::vector<MusicTrack> tracks, size_t start_index, bool loop = false,
                  std::string tag = "",
                  std::shared_ptr<MusicCache::Entry> prepared_entry = nullptr);
    std::string queue_tag() const;

    // Queue management. Indexes are 0-based. Callers restart playback (through
    // Application) when they change the current track.
    void GetQueue(std::vector<MusicTrack>& tracks, size_t& index) const;
    bool SelectIndex(size_t index);
    // Returns how many tracks were added (the queue holds at most 100).
    size_t AddToQueue(std::vector<MusicTrack> tracks, bool after_current);
    bool RemoveFromQueue(size_t index);
    void ClearQueue();
    bool HasTrack() const;
    bool GetCurrentTrack(MusicTrack& track) const;
    // Selects the next track. `automatic` means the current track ended by
    // itself, so repeat-one replays it and sequence mode stops at the end.
    bool MoveNext(bool automatic);
    bool MovePrevious();
    void SetPlayMode(MusicPlayMode mode);
    MusicPlayMode GetPlayMode() const;

    // Mounted folder with local music (for example "/sdcard"), or empty.
    void SetLocalRoot(std::string root);
    std::string GetLocalRoot() const;

    // Whether the user wants music to be heard once the device is free.
    void SetWantsPlayback(bool wants) { wants_playback_ = wants; }
    bool WantsPlayback() const { return wants_playback_; }

    // Resumes the paused session or starts the current track. Returns the
    // session id, or 0 on failure.
    uint32_t Play();
    void Pause();
    void Stop();
    bool IsPlaying() const;
    bool IsPaused() const;
    uint32_t session_id() const;

    // AudioService hooks.
    void OnPlaybackProgress(uint32_t playback_id, uint32_t media_position_ms);
    void OnPlaybackDrained();

    // Caller owns the returned object.
    cJSON* GetStatusJson() const;

private:
    struct Session;
    struct TaskContext;
    struct CacheJob {
        MusicTrack track;
        std::shared_ptr<MusicSource> source;
        std::string root;
    };

    AudioService& audio_service_;
    mutable std::mutex mutex_;
    std::shared_ptr<MusicSource> source_;
    std::vector<MusicTrack> queue_;
    // One selected, fully verified SD song stays pinned through the spoken
    // confirmation; ownership moves to its session before the reader starts.
    std::shared_ptr<MusicCache::Entry> prepared_entry_;
    size_t index_ = 0;
    MusicPlayMode play_mode_ = MusicPlayMode::kSequence;
    bool loop_queue_ = false;
    std::string queue_tag_;
    std::string local_root_;
    std::shared_ptr<Session> session_;
    std::vector<LyricLine> lyrics_;
    int lyric_index_ = -1;
    uint32_t position_ms_ = 0;
    uint32_t session_counter_ = 0;
    std::atomic<bool> wants_playback_{false};
    std::minstd_rand random_;
    LyricCallback on_lyric_;
    FinishedCallback on_finished_;
    std::mutex cache_jobs_mutex_;
    std::deque<CacheJob> cache_jobs_;
    std::optional<CacheJob> cache_active_job_;
    bool cache_worker_running_ = false;
    std::atomic<bool> cache_shutdown_{false};

    bool StartTasks(const std::shared_ptr<Session>& session);
    static void TaskEntry(void* arg);
    void NetTask(const std::shared_ptr<Session>& session);
    void ReadLocalFile(const std::shared_ptr<Session>& session);
    bool WaitForBuffer(Session& session, bool initial);
    void DecodeTask(const std::shared_ptr<Session>& session);
    std::unique_ptr<Http> OpenStream(Session& session, size_t offset, size_t& total_bytes,
                                     int connection_id = kMusicStreamConnectId,
                                     const std::function<bool()>& allowed = {});
    void QueueCacheJob(CacheJob job);
    static void CacheTaskEntry(void* arg);
    void CacheTask();
    bool CanDownloadCache() const;
    bool IsForegroundCacheJob(const CacheJob& job) const;
    bool IsActiveCacheJob(const MusicTrack& track, const std::string& base, const std::string& root);
    size_t WriteToBuffer(Session& session, const char* data, size_t size);
    bool PushFrame(Session& session, std::vector<int16_t>& pcm, uint32_t position_ms);
    void LoadLyrics(const std::shared_ptr<Session>& session);
    void CheckFinished(const std::shared_ptr<Session>& session);
    void ReportFailure(const std::shared_ptr<Session>& session, const std::string& error);
};

#endif  // MUSIC_PLAYER_H_
