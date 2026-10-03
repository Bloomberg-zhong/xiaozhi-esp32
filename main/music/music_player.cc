#include "music_player.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_random.h>
#include <cJSON.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "application.h"
#include "audio_codec.h"
#include "audio_service.h"
#include "board.h"
#include "esp_ae_rate_cvt.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "local_music.h"
#include "music_cache.h"
#include "music_util.h"

#define TAG "MusicPlayer"

namespace {

constexpr uint32_t kSessionIdFlag = 0x80000000;  // Keeps ids distinct from notify playback ids
constexpr uint32_t kNetTaskStackSize = 8192;
constexpr uint32_t kDecodeTaskStackSize = 8192;
// Both music tasks stay below the AFE task ("audio_afe", priority 3): echo
// cancellation and wake-word detection must never wait for the stream reader.
constexpr UBaseType_t kNetTaskPriority = 2;
constexpr UBaseType_t kDecodeTaskPriority = 2;
constexpr size_t kFallbackBufferSize = 32 * 1024;
constexpr size_t kNetChunkSize = 2048;
constexpr size_t kDecodeInputSize = 4096;
constexpr size_t kPrebufferBytes = 16 * 1024;
constexpr size_t kNetworkPrebufferBytes = 48 * 1024;
constexpr size_t kNetworkRebufferBytes = 32 * 1024;
constexpr int kStreamTimeoutMs = 10000;
constexpr int kReadPollMs = 1000;
constexpr int kMaxRedirects = 3;
constexpr int kMaxStreamRetries = 3;
constexpr int kMaxDecodeErrors = 8;
constexpr int kOutputFrameMs = 60;
constexpr size_t kMaxLocalLyricBytes = 64 * 1024;

#ifdef CONFIG_MUSIC_PLAYER_STREAM_BUFFER_KB
constexpr size_t kStreamBufferSize = CONFIG_MUSIC_PLAYER_STREAM_BUFFER_KB * 1024;
#else
constexpr size_t kStreamBufferSize = 256 * 1024;
#endif

void RegisterDecoders() {
    static std::once_flag once;
    std::call_once(once, []() {
        esp_mp3_dec_register();
        esp_aac_dec_register();
        esp_flac_dec_register();
        esp_wav_dec_register();
        esp_m4a_dec_register();
    });
}

esp_audio_simple_dec_type_t ToDecoderType(MusicAudioFormat format) {
    switch (format) {
        case MusicAudioFormat::kMp3:
            return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
        case MusicAudioFormat::kAac:
            return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
        case MusicAudioFormat::kM4a:
            return ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
        case MusicAudioFormat::kFlac:
            return ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
        case MusicAudioFormat::kWav:
            return ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
        default:
            return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
    }
}

}  // namespace

const char* MusicPlayModeName(MusicPlayMode mode) {
    switch (mode) {
        case MusicPlayMode::kRepeatAll:
            return "repeat_all";
        case MusicPlayMode::kRepeatOne:
            return "repeat_one";
        case MusicPlayMode::kShuffle:
            return "shuffle";
        default:
            return "sequence";
    }
}

bool ParseMusicPlayMode(const std::string& name, MusicPlayMode& mode) {
    for (auto candidate : {MusicPlayMode::kSequence, MusicPlayMode::kRepeatAll,
                           MusicPlayMode::kRepeatOne, MusicPlayMode::kShuffle}) {
        if (name == MusicPlayModeName(candidate)) {
            mode = candidate;
            return true;
        }
    }
    return false;
}

struct MusicPlayer::Session {
    uint32_t id = 0;
    MusicTrack track;
    std::shared_ptr<MusicSource> source;
    std::string local_root;
    std::shared_ptr<MusicCache::Entry> cache_entry;
    StreamBufferHandle_t buffer = nullptr;
    StaticStreamBuffer_t buffer_control = {};
    uint8_t* buffer_storage = nullptr;
    size_t buffer_capacity = 0;
    std::atomic<bool> local_stream{false};
    bool live_stream = false;
    std::atomic<bool> cancelled{false};
    std::atomic<bool> paused{false};
    std::atomic<bool> net_done{false};
    std::atomic<bool> net_failed{false};
    std::atomic<bool> decode_done{false};
    std::atomic<bool> finish_reported{false};
    std::mutex mutex;
    std::string content_type;
    std::string error;

    ~Session() {
        if (buffer != nullptr) {
            // This is a statically created stream: delete its RTOS state before
            // releasing our storage. IDF 6.0.2's DeleteWithCaps incorrectly
            // calls vSemaphoreDelete for streams and double-frees the control.
            vStreamBufferDelete(buffer);
        }
        heap_caps_free(buffer_storage);
    }
};

struct MusicPlayer::TaskContext {
    MusicPlayer* player;
    std::shared_ptr<Session> session;
    bool decoder;
};

MusicPlayer::MusicPlayer(AudioService& audio_service)
    : audio_service_(audio_service), random_(esp_random()) {}

MusicPlayer::~MusicPlayer() {
    cache_shutdown_ = true;
    Stop();
    // The worker never accesses a destroyed MusicPlayer. A blocked HTTP read
    // is bounded by kReadPollMs; connect/header operations use kStreamTimeoutMs.
    while (true) {
        {
            std::lock_guard<std::mutex> lock(cache_jobs_mutex_);
            if (!cache_worker_running_)
                break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void MusicPlayer::SetCallbacks(LyricCallback on_lyric, FinishedCallback on_finished) {
    std::lock_guard<std::mutex> lock(mutex_);
    on_lyric_ = std::move(on_lyric);
    on_finished_ = std::move(on_finished);
}

void MusicPlayer::SetSource(std::shared_ptr<MusicSource> source) {
    std::lock_guard<std::mutex> lock(mutex_);
    source_ = std::move(source);
}

std::shared_ptr<MusicSource> MusicPlayer::GetSource() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return source_;
}

void MusicPlayer::SetQueue(std::vector<MusicTrack> tracks, size_t start_index, bool loop,
                           std::string tag) {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_ = std::move(tracks);
    index_ = start_index < queue_.size() ? start_index : 0;
    loop_queue_ = loop;
    queue_tag_ = std::move(tag);
}

void MusicPlayer::GetQueue(std::vector<MusicTrack>& tracks, size_t& index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    tracks = queue_;
    index = index_;
}

bool MusicPlayer::SelectIndex(size_t index) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (index >= queue_.size()) {
        return false;
    }
    index_ = index;
    return true;
}

size_t MusicPlayer::AddToQueue(std::vector<MusicTrack> tracks, bool after_current) {
    std::lock_guard<std::mutex> lock(mutex_);
    constexpr size_t kMaxQueue = 100;
    size_t added = 0;
    size_t position = after_current && !queue_.empty() ? index_ + 1 : queue_.size();
    for (auto& track : tracks) {
        if (queue_.size() >= kMaxQueue) {
            break;
        }
        queue_.insert(queue_.begin() + position + added, std::move(track));
        ++added;
    }
    return added;
}

bool MusicPlayer::RemoveFromQueue(size_t index) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (index >= queue_.size()) {
        return false;
    }
    queue_.erase(queue_.begin() + index);
    if (index < index_) {
        --index_;
    } else if (index == index_ && index_ >= queue_.size()) {
        index_ = 0;
    }
    return true;
}

void MusicPlayer::ClearQueue() {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
    index_ = 0;
    queue_tag_.clear();
}

std::string MusicPlayer::queue_tag() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_tag_;
}

void MusicPlayer::SetLocalRoot(std::string root) {
    std::lock_guard<std::mutex> lock(mutex_);
    local_root_ = std::move(root);
}

std::string MusicPlayer::GetLocalRoot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return local_root_;
}

bool MusicPlayer::HasTrack() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !queue_.empty();
}

bool MusicPlayer::GetCurrentTrack(MusicTrack& track) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) {
        return false;
    }
    track = queue_[index_];
    return true;
}

bool MusicPlayer::MoveNext(bool automatic) {
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t size = queue_.size();
    if (size == 0) {
        return false;
    }
    switch (loop_queue_ ? MusicPlayMode::kRepeatAll : play_mode_) {
        case MusicPlayMode::kRepeatOne:
            if (automatic) {
                return true;
            }
            index_ = (index_ + 1) % size;
            return true;
        case MusicPlayMode::kRepeatAll:
            index_ = (index_ + 1) % size;
            return true;
        case MusicPlayMode::kShuffle:
            if (size > 1) {
                size_t next = random_() % (size - 1);
                index_ = next >= index_ ? next + 1 : next;
            }
            return true;
        case MusicPlayMode::kSequence:
        default:
            if (index_ + 1 < size) {
                ++index_;
                return true;
            }
            if (automatic) {
                return false;
            }
            index_ = 0;
            return true;
    }
}

bool MusicPlayer::MovePrevious() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) {
        return false;
    }
    index_ = index_ == 0 ? queue_.size() - 1 : index_ - 1;
    return true;
}

void MusicPlayer::SetPlayMode(MusicPlayMode mode) {
    std::lock_guard<std::mutex> lock(mutex_);
    play_mode_ = mode;
}

MusicPlayMode MusicPlayer::GetPlayMode() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return play_mode_;
}

uint32_t MusicPlayer::Play() {
    std::shared_ptr<Session> session;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (session_ && !session_->cancelled) {
            session = session_;
            session->paused = false;
        } else {
            if (queue_.empty()) {
                return 0;
            }
            session = std::make_shared<Session>();
            session->id = kSessionIdFlag | (++session_counter_ & ~kSessionIdFlag);
            session->track = queue_[index_];
            session->source = source_;
            session->local_root = local_root_;
            if (!StartTasks(session)) {
                return 0;
            }
            session_ = session;
            lyrics_.clear();
            lyric_index_ = -1;
            position_ms_ = 0;
            ESP_LOGI(TAG, "Playing %s - %s", session->track.title.c_str(),
                     session->track.artist.c_str());
            return session->id;
        }
    }
    // Resumed: the track may have been fully decoded while paused.
    CheckFinished(session);
    return session->id;
}

void MusicPlayer::Pause() {
    bool live;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!session_ || session_->paused) {
            return;
        }
        live = session_->track.live;
        session_->paused = !live;
    }
    if (live) {
        // A paused live stream would resume with seconds of stale audio, so drop
        // the session; Play() reopens the stream at the live position.
        Stop();
        return;
    }
    // Drop the few frames already queued so the pause is immediate.
    audio_service_.ResetDecoder();
}

void MusicPlayer::Stop() {
    std::shared_ptr<Session> session;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        session = std::move(session_);
        lyrics_.clear();
        lyric_index_ = -1;
        position_ms_ = 0;
    }
    if (session) {
        // The tasks notice the flag and release the session themselves.
        session->cancelled = true;
        audio_service_.ResetDecoder();
    }
}

bool MusicPlayer::IsPlaying() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return session_ && !session_->paused;
}

bool MusicPlayer::IsPaused() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return session_ && session_->paused;
}

uint32_t MusicPlayer::session_id() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return session_ ? session_->id : 0;
}

void MusicPlayer::OnPlaybackProgress(uint32_t playback_id, uint32_t media_position_ms) {
    std::string text;
    LyricCallback callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!session_ || session_->id != playback_id) {
            return;
        }
        position_ms_ = media_position_ms;
        int index = FindLyricIndex(lyrics_, media_position_ms);
        if (index < 0 || index == lyric_index_) {
            return;
        }
        lyric_index_ = index;
        text = lyrics_[index].text;
        callback = on_lyric_;
    }
    if (callback) {
        callback(playback_id, text);
    }
}

void MusicPlayer::OnPlaybackDrained() {
    std::shared_ptr<Session> session;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        session = session_;
    }
    if (session) {
        CheckFinished(session);
    }
}

void MusicPlayer::CheckFinished(const std::shared_ptr<Session>& session) {
    if (!session->decode_done || session->cancelled || session->paused ||
        session->finish_reported) {
        return;
    }
    if (!audio_service_.IsPlaybackIdle()) {
        return;  // Called again from OnPlaybackDrained()
    }
    if (session->finish_reported.exchange(true)) {
        return;
    }
    FinishedCallback callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback = on_finished_;
    }
    if (callback) {
        callback(session->id, true, "");
    }
}

void MusicPlayer::ReportFailure(const std::shared_ptr<Session>& session, const std::string& error) {
    if (session->cancelled || session->finish_reported.exchange(true)) {
        return;
    }
    ESP_LOGE(TAG, "Playback of %s failed: %s", session->track.title.c_str(), error.c_str());
    FinishedCallback callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback = on_finished_;
    }
    if (callback) {
        callback(session->id, false, error);
    }
}

cJSON* MusicPlayer::GetStatusJson() const {
    std::lock_guard<std::mutex> lock(mutex_);
    cJSON* root = cJSON_CreateObject();
    const char* state = !session_ ? "stopped" : session_->paused ? "paused" : "playing";
    cJSON_AddStringToObject(root, "state", state);
    cJSON_AddStringToObject(root, "play_mode", MusicPlayModeName(play_mode_));
    cJSON_AddNumberToObject(root, "queue_length", queue_.size());
    if (!queue_tag_.empty()) {
        cJSON_AddStringToObject(root, "queue", queue_tag_.c_str());
    }
    cJSON_AddBoolToObject(root, "source_configured", source_ != nullptr);
    if (!queue_.empty()) {
        const MusicTrack& track = queue_[index_];
        cJSON_AddNumberToObject(root, "queue_position", index_ + 1);
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "title", track.title.c_str());
        cJSON_AddStringToObject(item, "artist", track.artist.c_str());
        cJSON_AddStringToObject(item, "album", track.album.c_str());
        if (!track.provider.empty()) {
            cJSON_AddStringToObject(item, "source", track.provider.c_str());
        }
        if (track.live) {
            cJSON_AddBoolToObject(item, "live", true);
        }
        if (track.duration_ms > 0) {
            cJSON_AddNumberToObject(item, "duration_s", track.duration_ms / 1000);
        }
        cJSON_AddItemToObject(root, "track", item);
        if (session_) {
            cJSON_AddNumberToObject(root, "position_s", position_ms_ / 1000);
        }
    }
    return root;
}

bool MusicPlayer::StartTasks(const std::shared_ptr<Session>& session) {
    size_t buffer_size = kStreamBufferSize;
    session->buffer_storage =
        static_cast<uint8_t*>(heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (session->buffer_storage == nullptr) {
        buffer_size = kFallbackBufferSize;
        session->buffer_storage = static_cast<uint8_t*>(
            heap_caps_malloc(buffer_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    if (session->buffer_storage == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate the stream buffer");
        return false;
    }
    session->buffer = xStreamBufferCreateStatic(buffer_size, 1, session->buffer_storage,
                                                &session->buffer_control);
    if (session->buffer == nullptr) {
        ESP_LOGE(TAG, "Failed to create the stream buffer");
        return false;
    }
    // Static stream buffers keep one byte empty to distinguish full from empty.
    session->buffer_capacity = buffer_size - 1;
    session->local_stream = IsLocalMusicPath(session->track.stream_url);
    session->live_stream = session->track.live;

    auto* net_context = new TaskContext{this, session, false};
    if (xTaskCreate(TaskEntry, "music_net", kNetTaskStackSize, net_context, kNetTaskPriority,
                    nullptr) != pdPASS) {
        delete net_context;
        ESP_LOGE(TAG, "Failed to create the music network task (internal free=%u largest=%u)",
                 unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
        return false;
    }
    auto* decode_context = new TaskContext{this, session, true};
    if (xTaskCreate(TaskEntry, "music_dec", kDecodeTaskStackSize, decode_context,
                    kDecodeTaskPriority, nullptr) != pdPASS) {
        delete decode_context;
        session->cancelled = true;  // Stops the network task
        ESP_LOGE(TAG, "Failed to create the music decoder task (internal free=%u largest=%u)",
                 unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
        return false;
    }
    return true;
}

void MusicPlayer::TaskEntry(void* arg) {
    auto* context = static_cast<TaskContext*>(arg);
    if (context->decoder) {
        context->player->DecodeTask(context->session);
    } else {
        context->player->NetTask(context->session);
    }
    delete context;
    vTaskDelete(nullptr);
}

std::unique_ptr<Http> MusicPlayer::OpenStream(Session& session, size_t offset, size_t& total_bytes,
                                              int connection_id,
                                              const std::function<bool()>& allowed) {
    const auto continuing = [&] { return !session.cancelled && (!allowed || allowed()); };
    auto network = Board::GetInstance().GetNetwork();
    std::string url = session.track.stream_url;
    for (int redirect = 0; redirect <= kMaxRedirects && continuing(); ++redirect) {
        if (!IsHttpUrl(url)) {
            ESP_LOGE(TAG, "Invalid stream URL");
            return nullptr;
        }
        auto http = network->CreateHttp(connection_id);
        if (!http) {
            return nullptr;
        }
        http->SetTimeout(kStreamTimeoutMs);
        http->SetHeader("Accept-Encoding", "identity");
        if (offset > 0) {
            http->SetHeader("Range", "bytes=" + std::to_string(offset) + "-");
        }
        if (session.source) {
            session.source->ApplyHeaders(url, *http);
        }
        auto opened = http->Open("GET", url);
        if (!opened) {
            ESP_LOGW(TAG, "Failed to open the music stream: %s", opened.error().ToString().c_str());
            return nullptr;
        }
        auto status_result = http->GetStatusCode();
        if (!status_result) {
            http->Close();
            return nullptr;
        }
        const int status = *status_result;
        if (!continuing()) {
            http->Close();
            return nullptr;
        }
        if (status >= 300 && status < 400) {
            std::string location = http->GetResponseHeader("Location");
            http->Close();
            if (location.empty()) {
                return nullptr;
            }
            url = ResolveUrl(url, location);
            continue;
        }
        const size_t body_length = http->GetBodyLength();
        if (status == 206 && offset > 0) {
            unsigned long long start = 0, end = 0, length = 0;
            char tail = 0;
            const auto range = http->GetResponseHeader("Content-Range");
            if (std::sscanf(range.c_str(), "bytes %llu-%llu/%llu%c", &start, &end, &length,
                            &tail) != 3 ||
                start != offset || end < start || length <= end || length > SIZE_MAX ||
                (body_length > 0 && body_length != end - start + 1)) {
                ESP_LOGW(TAG, "Invalid music HTTP Content-Range");
                http->Close();
                return nullptr;
            }
            total_bytes = static_cast<size_t>(length);
            return http;
        }
        if (status < 200 || status >= 300) {
            ESP_LOGW(TAG, "Music stream returned HTTP %d", status);
            http->Close();
            return nullptr;
        }
        total_bytes = body_length;
        if (offset == 0) {
            std::lock_guard<std::mutex> lock(session.mutex);
            session.content_type = http->GetResponseHeader("Content-Type");
            return http;
        }
        // The server ignored the Range header: skip what was already buffered.
        char discard[512];
        size_t skipped = 0;
        http->SetTimeout(kReadPollMs);
        const TickType_t started = xTaskGetTickCount();
        while (skipped < offset && continuing()) {
            if (xTaskGetTickCount() - started >= pdMS_TO_TICKS(kStreamTimeoutMs)) {
                http->Close();
                return nullptr;
            }
            auto read = http->Read(discard, std::min(sizeof(discard), offset - skipped));
            int size = read ? *read : -1;
            if (size <= 0) {
                http->Close();
                return nullptr;
            }
            skipped += size;
        }
        if (skipped != offset || !continuing()) {
            http->Close();
            return nullptr;
        }
        return http;
    }
    return nullptr;
}

size_t MusicPlayer::WriteToBuffer(Session& session, const char* data, size_t size) {
    size_t sent = 0;
    while (sent < size) {
        if (session.cancelled || session.paused) {
            break;
        }
        sent += xStreamBufferSend(session.buffer, data + sent, size - sent, pdMS_TO_TICKS(100));
    }
    return sent;
}

void MusicPlayer::LoadLyrics(const std::shared_ptr<Session>& session) {
    std::vector<LyricLine> lines;
    const MusicTrack& track = session->track;
    if (IsLocalMusicPath(track.lyric_url)) {
        std::string text;
        if (ReadLocalTextFile(track.lyric_url, kMaxLocalLyricBytes, text)) {
            lines = ParseLrc(text);
        }
    } else if (!track.lyric_text.empty()) {
        lines = ParseLrc(track.lyric_text);
    } else if (session->source && !IsLocalMusicPath(track.stream_url)) {
        MusicCache cache(session->local_root);
        std::string text;
        const auto& base = session->source->base_url();
        if (cache.ReadLyrics(track, base, text)) {
            lines = ParseLrc(text);
        }
        if (lines.empty()) {
            lines = session->source->FetchLyrics(track);
            if (!lines.empty()) {
                cache.StoreLyrics(track, base, lines);
            }
        }
    }
    if (lines.empty()) {
        return;
    }
    ESP_LOGI(TAG, "Loaded %u lyric lines", static_cast<unsigned>(lines.size()));
    std::lock_guard<std::mutex> lock(mutex_);
    if (session_ == session) {
        lyrics_ = std::move(lines);
        lyric_index_ = -1;
    }
}

void MusicPlayer::ReadLocalFile(const std::shared_ptr<Session>& session) {
    session->local_stream = true;
    FILE* file = std::fopen(session->track.stream_url.c_str(), "rb");
    if (file == nullptr) {
        std::lock_guard<std::mutex> lock(session->mutex);
        session->error = "cannot open the music file";
        session->net_failed = true;
        return;
    }
    ESP_LOGI(TAG, "Reading SD song: %s", session->track.stream_url.c_str());
    // Local lyrics are ready before audio buffering can wait for playback to
    // consume a whole song. LoadLyrics never fetches from the network here.
    if (!session->cancelled) {
        LoadLyrics(session);
    }
    std::vector<char> chunk(kNetChunkSize);
    bool failed = false;
    while (!session->cancelled) {
        size_t size = std::fread(chunk.data(), 1, chunk.size(), file);
        if (size == 0) {
            failed = std::ferror(file) != 0;
            break;
        }
        size_t sent = 0;
        while (sent < size && !session->cancelled) {
            sent += WriteToBuffer(*session, chunk.data() + sent, size - sent);
            if (sent < size && !session->cancelled) {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        }
    }
    std::fclose(file);
    if (failed) {
        std::lock_guard<std::mutex> lock(session->mutex);
        session->error = "cannot read the music file";
        session->net_failed = true;
        return;
    }
    session->net_done = true;
}

void MusicPlayer::NetTask(const std::shared_ptr<Session>& session) {
    MusicCache cache(session->local_root);
    if (IsLocalMusicPath(session->track.stream_url)) {
        session->cache_entry = cache.PinLocal(session->track.stream_url);
        if (MusicCache::IsManagedPath(session->track.stream_url) && !session->cache_entry) {
            std::lock_guard<std::mutex> lock(session->mutex);
            session->error = "cached music file is incomplete or damaged";
            session->net_failed = true;
            return;
        }
        ReadLocalFile(session);
        return;
    }
    const std::string source_base = session->source ? session->source->base_url() : "";
    // A previous reader or the background worker must relinquish this song
    // before the foreground opens its own HTTP request.
    for (int waited = 0; cache.IsWriting(session->track, source_base) ||
                         IsActiveCacheJob(session->track, source_base, session->local_root);
         ++waited) {
        if (session->cancelled)
            return;
        if (waited >= 750) {
            std::lock_guard<std::mutex> lock(session->mutex);
            session->error = "the song cache is still busy";
            session->net_failed = true;
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (auto entry = cache.Find(session->track, source_base)) {
        session->cache_entry = entry;
        session->track = entry->track;
        ESP_LOGI(TAG, "Playing cached song: %s", session->track.title.c_str());
        ReadLocalFile(session);
        return;
    }
    // Prepare optional metadata before audio starts. FetchLyrics can wait for
    // an HTTP timeout; doing this inside the body loop starves the decoder.
    if (!session->track.live && !session->cancelled) {
        ESP_LOGI(TAG, "Preparing music lyrics before audio buffering");
        LoadLyrics(session);
    }
    if (session->cancelled) {
        return;
    }
    if (session->source && !session->local_root.empty()) {
        for (auto track : cache.Pending(source_base)) {
            if (MusicCache::SameIdentity(track, source_base, session->track, source_base))
                continue;  // This reader owns its resume, never queue a second download.
            auto rebuilt = session->source->BuildTrack(track.id, track.title, track.artist, false);
            rebuilt.album = track.album;
            rebuilt.provider = track.provider;
            rebuilt.duration_ms = track.duration_ms;
            rebuilt.lyric_text = track.lyric_text;
            QueueCacheJob({std::move(rebuilt), session->source, session->local_root});
        }
    }
    std::unique_ptr<MusicCache::Writer> cache_writer;
    bool cache_attempted = false;
    std::vector<char> chunk(kNetChunkSize);
    size_t received = 0;
    size_t total = 0;
    int failures = 0;
    bool finished = false;
    // A live stream has no end and cannot be resumed at an offset: a dropped
    // connection is simply reopened.
    const bool live = session->track.live;

    while (!session->cancelled && !finished) {
        if (session->paused) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        bool suspended = false;
        auto http = OpenStream(*session, live ? 0 : received, total);
        if (http) {
            if (!cache_attempted && !live) {
                cache_attempted = true;
                cache_writer = cache.Begin(session->track, source_base, total);
                if (!cache_writer && !session->local_root.empty()) {
                    ESP_LOGW(
                        TAG,
                        "SD cache unavailable (unknown length, space or active job); streaming");
                }
                if (cache_writer && cache_writer->offset() > 0) {
                    // Feed the validated prefix from SD and request only the
                    // missing suffix, instead of downloading the old bytes again.
                    http->Close();
                    FILE* prefix = std::fopen(cache_writer->PartialPath().c_str(), "rb");
                    bool ok = prefix != nullptr;
                    const size_t cached = cache_writer->offset();
                    while (ok && received < cached && !session->cancelled) {
                        if (session->paused) {
                            vTaskDelay(pdMS_TO_TICKS(20));
                            continue;
                        }
                        // WriteToBuffer may return only part of a chunk when
                        // pause arrives; seek by consumed bytes on every read.
                        if (std::fseek(prefix, static_cast<long>(received), SEEK_SET) != 0) {
                            ok = false;
                            break;
                        }
                        const size_t size = std::fread(
                            chunk.data(), 1, std::min(chunk.size(), cached - received), prefix);
                        if (size == 0) {
                            ok = false;
                            break;
                        }
                        received += WriteToBuffer(*session, chunk.data(), size);
                    }
                    if (prefix)
                        std::fclose(prefix);
                    if (!ok) {
                        std::lock_guard<std::mutex> lock(session->mutex);
                        session->error = "cannot read the cached music prefix";
                        session->net_failed = true;
                        return;
                    }
                    ESP_LOGI(TAG, "Reused SD prefix: %u bytes", static_cast<unsigned>(received));
                    finished = received == total;
                    continue;
                }
            }
            // Keep connect/header timeouts long, but bound a blocked body read
            // so pause/stop can release RX resources promptly.
            http->SetTimeout(kReadPollMs);
            int read_timeouts = 0;
            while (!session->cancelled) {
                if (session->paused) {
                    suspended = true;
                    break;
                }
                auto read = http->Read(chunk.data(), chunk.size());
                if (!read && read.error().code == NetworkErrc::Timeout &&
                    ++read_timeouts < kStreamTimeoutMs / kReadPollMs) {
                    continue;
                }
                read_timeouts = 0;
                int size = read ? *read : -1;
                if (size < 0) {
                    break;
                }
                if (size == 0) {
                    // A close before Content-Length is reached is a dropped connection.
                    finished = !live && (total == 0 || received >= total);
                    break;
                }
                failures = 0;
                const size_t written = WriteToBuffer(*session, chunk.data(), size);
                if (cache_writer && written > 0) {
                    const size_t cached = cache_writer->offset();
                    const size_t skip =
                        cached > received ? std::min(written, cached - received) : 0;
                    if (skip < written &&
                        !cache_writer->Append(received + skip, chunk.data() + skip,
                                              written - skip)) {
                        ESP_LOGW(TAG, "SD cache write failed; streaming continues");
                        cache_writer.reset();
                    }
                }
                received += written;
                if (written != static_cast<size_t>(size)) {
                    suspended = !session->cancelled;
                    break;
                }
                if (total > 0 && received >= total) {
                    finished = true;
                    break;
                }
            }
            http->Close();
        }
        if (finished || session->cancelled) {
            break;
        }
        if (suspended || session->paused) {
            // A paused TCP stream can retain all of a small board's Wi-Fi RX
            // buffers and starve the conversation's UDP packets. Release the
            // connection; retain the decoder/buffer and resume at bytes actually
            // enqueued, including a partially written chunk.
            ESP_LOGI(TAG, "Suspended music download at %u bytes", static_cast<unsigned>(received));
            continue;
        }
        if (++failures > kMaxStreamRetries) {
            std::lock_guard<std::mutex> lock(session->mutex);
            session->error =
                received == 0 ? "cannot open the music stream" : "the music stream was interrupted";
            session->net_failed = true;
            if (cache_writer && cache_writer->Suspend()) {
                QueueCacheJob({session->track, session->source, session->local_root});
            }
            return;
        }
        ESP_LOGW(TAG, "Reconnecting the music stream at %u bytes (attempt %d)",
                 static_cast<unsigned>(received), failures);
        vTaskDelay(pdMS_TO_TICKS(500 * failures));
    }

    if (cache_writer) {
        if (finished && !session->cancelled) {
            const bool saved = cache_writer->Finish();
            ESP_LOGI(TAG, "SD cache complete: %s (%s)", session->track.title.c_str(),
                     saved ? "saved" : "failed integrity or card write");
            (void)saved;
        } else {
            const bool retained = cache_writer->Suspend();
            ESP_LOGI(TAG, "SD cache queued for completion: %s (%s)", session->track.title.c_str(),
                     retained ? "resumable partial"
                              : "pending publication failed; recovery will be checked");
            (void)retained;
            QueueCacheJob({session->track, session->source, session->local_root});
        }
    }
    session->net_done = true;
}

bool MusicPlayer::CanDownloadCache() const {
    const auto state = Application::GetInstance().GetDeviceState();
    if (cache_shutdown_ || (state != kDeviceStateIdle && state != kDeviceStatePlaying))
        return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (session_ && session_->paused)
        return false;
    // Give the audible stream the Wi-Fi receive buffers and heap headroom.
    // Complete old jobs once the foreground reader finishes or plays from SD.
    return !session_ || session_->cancelled || session_->net_done ||
           (!queue_.empty() && IsLocalMusicPath(queue_[index_].stream_url));
}

bool MusicPlayer::IsForegroundCacheJob(const CacheJob& job) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!session_ || session_->cancelled || queue_.empty() || session_->local_root != job.root)
        return false;
    const auto& current = queue_[index_];
    const std::string current_base = session_->source ? session_->source->base_url() : "";
    const std::string job_base = job.source ? job.source->base_url() : "";
    return MusicCache::SameIdentity(current, current_base, job.track, job_base);
}

bool MusicPlayer::IsActiveCacheJob(const MusicTrack& track, const std::string& base,
                                   const std::string& root) {
    std::lock_guard<std::mutex> lock(cache_jobs_mutex_);
    return cache_active_job_ && cache_active_job_->root == root &&
           MusicCache::SameIdentity(
               cache_active_job_->track,
               cache_active_job_->source ? cache_active_job_->source->base_url() : "", track, base);
}

void MusicPlayer::QueueCacheJob(CacheJob job) {
    if (cache_shutdown_ || job.root.empty() || job.track.live || !IsHttpUrl(job.track.stream_url)) {
        return;
    }
    std::lock_guard<std::mutex> lock(cache_jobs_mutex_);
    auto same = [&job](const CacheJob& queued) {
        return queued.root == job.root &&
               MusicCache::SameIdentity(queued.track,
                                        queued.source ? queued.source->base_url() : "", job.track,
                                        job.source ? job.source->base_url() : "");
    };
    if (cache_active_job_ && same(*cache_active_job_) &&
        MusicCache(job.root).IsWriting(job.track, job.source ? job.source->base_url() : ""))
        return;
    for (const auto& queued : cache_jobs_) {
        if (same(queued)) {
            return;
        }
    }
    if (cache_jobs_.size() >= 32) {
        ESP_LOGW(TAG, "SD cache backlog full; pending song retained on card");
        return;
    }
    cache_jobs_.push_back(std::move(job));
    if (!cache_worker_running_) {
        cache_worker_running_ = true;
        if (xTaskCreate(CacheTaskEntry, "music_cache", kNetTaskStackSize, this, 1, nullptr) !=
            pdPASS) {
            cache_worker_running_ = false;
            ESP_LOGW(TAG, "Cannot start SD cache worker; pending songs retained on card");
        }
    }
}

void MusicPlayer::CacheTaskEntry(void* arg) {
    static_cast<MusicPlayer*>(arg)->CacheTask();
    vTaskDelete(nullptr);
}

void MusicPlayer::CacheTask() {
    // Exactly one low-priority worker, one 2 KiB body buffer, and at most 32
    // pending jobs. It releases HTTP during conversations, just like NetTask.
    std::vector<char> chunk(kNetChunkSize);
    while (!cache_shutdown_) {
        CacheJob job;
        {
            std::lock_guard<std::mutex> lock(cache_jobs_mutex_);
            cache_active_job_.reset();
            if (cache_jobs_.empty()) {
                cache_worker_running_ = false;
                return;
            }
            job = std::move(cache_jobs_.front());
            cache_jobs_.pop_front();
            cache_active_job_ = job;
        }
        MusicCache cache(job.root);
        const std::string base = job.source ? job.source->base_url() : "";
        if (cache.Find(job.track, base))
            continue;
        if (IsForegroundCacheJob(job))
            continue;
        auto session = std::make_shared<Session>();
        session->track = job.track;
        session->source = job.source;
        session->local_root = job.root;
        std::unique_ptr<MusicCache::Writer> writer;
        size_t offset = 0, total = 0;
        int failures = 0;
        bool complete = false;
        while (!cache_shutdown_ && !complete) {
            if (IsForegroundCacheJob(job))
                break;
            if (!CanDownloadCache()) {
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            // ID 8 is distinct from stream/API/weather/artwork modem channels.
            auto http = OpenStream(*session, offset, total, 8, [&] {
                return !cache_shutdown_ && CanDownloadCache() && !IsForegroundCacheJob(job);
            });
            if (http) {
                if (IsForegroundCacheJob(job)) {
                    http->Close();
                    break;
                }
                if (!writer) {
                    writer = cache.Begin(job.track, base, total);
                    if (!writer) {
                        http->Close();
                        break;
                    }
                    if (writer->offset() != offset) {
                        offset = writer->offset();
                        http->Close();
                        if (offset == total) {
                            complete = true;
                            break;  // All bytes are already validated on SD; no Range past EOF.
                        }
                        continue;  // Reopen at the validated persistent partial offset.
                    }
                }
                http->SetTimeout(kReadPollMs);
                int timeouts = 0;
                while (!cache_shutdown_ && CanDownloadCache() && !IsForegroundCacheJob(job)) {
                    auto read = http->Read(chunk.data(), chunk.size());
                    if (!read && read.error().code == NetworkErrc::Timeout &&
                        ++timeouts < kStreamTimeoutMs / kReadPollMs)
                        continue;
                    timeouts = 0;
                    int size = read ? *read : -1;
                    if (size <= 0) {
                        complete = size == 0 && total > 0 && offset == total;
                        break;
                    }
                    if (!writer->Append(offset, chunk.data(), size))
                        break;
                    offset += size;
                    failures = 0;
                    if (total > 0 && offset == total) {
                        complete = true;
                        break;
                    }
                }
                http->Close();
            }
            if (complete || cache_shutdown_)
                break;
            if (IsForegroundCacheJob(job))
                break;
            if (!CanDownloadCache())
                continue;
            if (++failures > kMaxStreamRetries)
                break;
            vTaskDelay(pdMS_TO_TICKS(500 * failures));
        }
        if (!writer) {
            ESP_LOGW(TAG, "Background SD cache unavailable: %s (network, card or space)",
                     job.track.title.c_str());
        }
        if (writer) {
            if (complete) {
                const bool saved = writer->Finish();
                ESP_LOGI(TAG, "Background SD cache: %s (%s)", job.track.title.c_str(),
                         saved ? "saved" : "failed integrity or space");
                (void)saved;
            } else {
                const bool retained = writer->Suspend();
                ESP_LOGW(TAG, "Background SD cache unfinished: %s (%s)", job.track.title.c_str(),
                         retained ? "pending on card"
                                  : "pending publication failed; recovery will be checked");
                (void)retained;
            }
        }
    }
    std::lock_guard<std::mutex> lock(cache_jobs_mutex_);
    cache_active_job_.reset();
    cache_worker_running_ = false;
}

bool MusicPlayer::PushFrame(Session& session, std::vector<int16_t>& pcm, uint32_t position_ms) {
    while (!session.cancelled) {
        if (session.paused) {
            // Keep the frame and deliver it after resume.
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (audio_service_.PushPcmToPlaybackQueue(pcm, session.id, position_ms, true)) {
            return true;
        }
        // Rejected because the playback queue was reset (pause or stop).
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return false;
}

bool MusicPlayer::WaitForBuffer(Session& session, bool initial) {
    const TickType_t started = xTaskGetTickCount();
    if (!initial) {
        ESP_LOGW(TAG, "Music stream underrun; refilling audio buffer");
    }
    while (!session.cancelled && !session.net_done && !session.net_failed) {
        // Cache lookup can switch an online session to a local reader while we
        // wait. Keep card playback and live radio responsive, and reserve one
        // writer chunk even when PSRAM allocation fell back to a small buffer.
        const size_t desired = session.local_stream || session.live_stream
                                   ? kPrebufferBytes
                                   : (initial ? kNetworkPrebufferBytes : kNetworkRebufferBytes);
        const size_t margin = std::min(kNetChunkSize, session.buffer_capacity / 4);
        const size_t target = std::min(desired, session.buffer_capacity - margin);
        if (!session.paused && xStreamBufferBytesAvailable(session.buffer) >= target) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (session.cancelled) {
        return false;
    }
    const auto elapsed_ms = (xTaskGetTickCount() - started) * portTICK_PERIOD_MS;
    ESP_LOGI(TAG, "%s audio buffer: %u bytes after %u ms", initial ? "Initial" : "Recovered",
             static_cast<unsigned>(xStreamBufferBytesAvailable(session.buffer)),
             static_cast<unsigned>(elapsed_ms));
    (void)elapsed_ms;
    return true;
}

void MusicPlayer::DecodeTask(const std::shared_ptr<Session>& session) {
    auto* codec = Board::GetInstance().GetAudioCodec();
    const int output_rate = codec->output_sample_rate();
    const size_t frame_samples = static_cast<size_t>(output_rate) * kOutputFrameMs / 1000;

    // Start ordinary online tracks with a useful reserve against short stalls.
    if (!WaitForBuffer(*session, true)) {
        return;
    }

    std::vector<uint8_t> input(kDecodeInputSize);
    size_t input_length = 0;
    while (!session->cancelled && input_length < 12) {
        size_t size = xStreamBufferReceive(session->buffer, input.data() + input_length,
                                           input.size() - input_length, pdMS_TO_TICKS(50));
        input_length += size;
        if (size == 0 && (session->net_done || session->net_failed) &&
            xStreamBufferIsEmpty(session->buffer)) {
            break;
        }
    }
    if (session->cancelled) {
        return;
    }
    if (input_length == 0) {
        std::string error;
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            error = session->error.empty() ? "the music stream is empty" : session->error;
        }
        ReportFailure(session, error);
        return;
    }

    std::string content_type;
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        content_type = session->content_type;
    }
    MusicAudioFormat format =
        DetectMusicAudioFormat(input.data(), input_length, content_type, session->track.stream_url);
    if (format == MusicAudioFormat::kUnknown) {
        ReportFailure(session, "unsupported audio format");
        return;
    }
    ESP_LOGI(TAG, "Decoding %s stream", MusicAudioFormatName(format));

    RegisterDecoders();
    esp_audio_simple_dec_cfg_t decoder_config = {
        .dec_type = ToDecoderType(format),
        .dec_cfg = nullptr,
        .cfg_size = 0,
        .use_frame_dec = false,
    };
    esp_audio_simple_dec_handle_t decoder = nullptr;
    if (esp_audio_simple_dec_open(&decoder_config, &decoder) != ESP_AUDIO_ERR_OK ||
        decoder == nullptr) {
        ReportFailure(session, "cannot open the audio decoder");
        return;
    }

    std::vector<uint8_t> output(8192);
    std::vector<int16_t> mono;
    std::vector<int16_t> resampled;
    std::vector<int16_t> pending;
    pending.reserve(frame_samples * 2);
    esp_ae_rate_cvt_handle_t resampler = nullptr;
    uint32_t resampler_rate = 0;
    uint64_t output_samples = 0;
    bool end_of_stream = false;
    bool need_more_data = false;
    int errors = 0;
    std::string failure;

    while (!session->cancelled) {
        if (session->paused) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (!end_of_stream && input_length < input.size()) {
            TickType_t wait = (input_length == 0 || need_more_data) ? pdMS_TO_TICKS(50) : 0;
            size_t size = xStreamBufferReceive(session->buffer, input.data() + input_length,
                                               input.size() - input_length, wait);
            input_length += size;
            if (size == 0 && xStreamBufferIsEmpty(session->buffer)) {
                if (session->net_failed) {
                    std::lock_guard<std::mutex> lock(session->mutex);
                    failure = session->error;
                    break;
                }
                if (session->net_done) {
                    end_of_stream = true;
                } else if (input_length == 0 || need_more_data) {
                    // Preserve the parser, its partial input and pending PCM.
                    // Resuming on every small packet would turn a single stall
                    // into repeated audible gaps.
                    if (!WaitForBuffer(*session, false)) {
                        break;
                    }
                    continue;
                }
            }
        }
        if (input_length == 0 && !end_of_stream) {
            continue;
        }

        esp_audio_simple_dec_raw_t raw = {
            .buffer = input.data(),
            .len = static_cast<uint32_t>(input_length),
            .eos = end_of_stream,
            .consumed = 0,
            .frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE,
        };
        esp_audio_simple_dec_out_t frame = {
            .buffer = output.data(),
            .len = static_cast<uint32_t>(output.size()),
            .needed_size = 0,
            .decoded_size = 0,
        };
        esp_audio_err_t ret = esp_audio_simple_dec_process(decoder, &raw, &frame);
        if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH && frame.needed_size > output.size()) {
            output.resize(frame.needed_size);
            continue;
        }
        if (ret == ESP_AUDIO_ERR_DATA_LACK || ret == ESP_AUDIO_ERR_CONTINUE) {
            ret = ESP_AUDIO_ERR_OK;  // The parser keeps partial frames and waits for more input
        }
        if (ret != ESP_AUDIO_ERR_OK) {
            if (end_of_stream) {
                break;
            }
            if (++errors > kMaxDecodeErrors) {
                failure = "audio decoding failed";
                break;
            }
            // Drop the chunk; the parser resynchronizes on the next frame.
            input_length = 0;
            need_more_data = false;
            continue;
        }

        const size_t consumed = std::min<size_t>(raw.consumed, input_length);
        if (consumed > 0) {
            std::memmove(input.data(), input.data() + consumed, input_length - consumed);
            input_length -= consumed;
        }
        need_more_data = consumed == 0 && frame.decoded_size == 0;
        if (need_more_data && input_length == input.size()) {
            // The decoder cannot use a full input buffer: treat it as corrupt.
            input_length = 0;
            if (++errors > kMaxDecodeErrors) {
                failure = "audio decoding failed";
                break;
            }
        }
        if (frame.decoded_size == 0) {
            if (end_of_stream && (input_length == 0 || consumed == 0)) {
                break;  // Fully flushed
            }
            continue;
        }
        errors = 0;

        esp_audio_simple_dec_info_t info = {};
        if (esp_audio_simple_dec_get_info(decoder, &info) != ESP_AUDIO_ERR_OK ||
            info.sample_rate == 0 || info.channel == 0) {
            continue;
        }
        mono.clear();
        DownmixToMono16(frame.buffer, frame.decoded_size, info.channel, info.bits_per_sample, mono);

        const std::vector<int16_t>* samples = &mono;
        if (static_cast<int>(info.sample_rate) != output_rate) {
            if (resampler == nullptr || resampler_rate != info.sample_rate) {
                if (resampler != nullptr) {
                    esp_ae_rate_cvt_close(resampler);
                    resampler = nullptr;
                }
                esp_ae_rate_cvt_cfg_t config = {
                    .src_rate = info.sample_rate,
                    .dest_rate = static_cast<uint32_t>(output_rate),
                    .channel = 1,
                    .bits_per_sample = ESP_AUDIO_BIT16,
                    .complexity = 2,
                    .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,
                };
                if (esp_ae_rate_cvt_open(&config, &resampler) != ESP_AE_ERR_OK ||
                    resampler == nullptr) {
                    resampler = nullptr;
                    failure = "unsupported sample rate";
                    break;
                }
                resampler_rate = info.sample_rate;
            }
            uint32_t capacity = 0;
            esp_ae_rate_cvt_get_max_out_sample_num(resampler, mono.size(), &capacity);
            resampled.resize(capacity);
            uint32_t produced = capacity;
            esp_ae_rate_cvt_process(resampler, reinterpret_cast<esp_ae_sample_t>(mono.data()),
                                    mono.size(),
                                    reinterpret_cast<esp_ae_sample_t>(resampled.data()), &produced);
            resampled.resize(produced);
            samples = &resampled;
        }

        pending.insert(pending.end(), samples->begin(), samples->end());
        size_t offset = 0;
        while (pending.size() - offset >= frame_samples && !session->cancelled) {
            std::vector<int16_t> pcm(pending.begin() + offset,
                                     pending.begin() + offset + frame_samples);
            uint32_t position_ms = static_cast<uint32_t>(output_samples * 1000 / output_rate);
            if (!PushFrame(*session, pcm, position_ms)) {
                break;
            }
            output_samples += frame_samples;
            offset += frame_samples;
        }
        pending.erase(pending.begin(), pending.begin() + offset);
    }

    if (!session->cancelled && failure.empty() && !pending.empty()) {
        uint32_t position_ms = static_cast<uint32_t>(output_samples * 1000 / output_rate);
        PushFrame(*session, pending, position_ms);
    }
    if (resampler != nullptr) {
        esp_ae_rate_cvt_close(resampler);
    }
    esp_audio_simple_dec_close(decoder);

    if (session->cancelled) {
        return;
    }
    if (!failure.empty()) {
        ReportFailure(session, failure);
        return;
    }
    session->decode_done = true;
    CheckFinished(session);
}
