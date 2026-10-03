#include "music_cover_loader.h"

#if CONFIG_USE_MUSIC_PLAYER
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>

#include "application.h"
#include "board.h"
#include "jpeg_to_image.h"
#include "music_cache.h"
#include "music_cover_model.h"
#include "music_util.h"

namespace {
constexpr size_t kMaxCoverBytes = 64 * 1024;
struct CoverRequest {
    uint32_t session;
    uint32_t revision;
    MusicTrack track;
    std::shared_ptr<MusicSource> source;
    std::string root;
    unsigned retries = 0;
    int64_t retry_after = 0;
};

bool ReadCover(const std::string& path, std::string& bytes) {
    std::unique_ptr<FILE, decltype(&fclose)> file(fopen(path.c_str(), "rb"), fclose);
    if (!file || fseek(file.get(), 0, SEEK_END) != 0)
        return false;
    const long size = ftell(file.get());
    if (size <= 0 || size > static_cast<long>(kMaxCoverBytes) ||
        fseek(file.get(), 0, SEEK_SET) != 0)
        return false;
    bytes.resize(size);
    return fread(bytes.data(), 1, bytes.size(), file.get()) == bytes.size();
}

enum class CoverDownload { kReady, kTransientFailure, kPermanentFailure, kInterrupted };

CoverDownload DownloadCover(const CoverRequest& request, std::string& bytes,
                            const std::function<bool()>& current) {
    auto* network = Board::GetInstance().GetNetwork();
    if (!network)
        return CoverDownload::kTransientFailure;
    std::string url = request.track.cover_url;
    const int64_t deadline = esp_timer_get_time() + 8000000;
    auto available = [&] { return current() && esp_timer_get_time() < deadline; };
    auto failed = [&] {
        return current() ? CoverDownload::kTransientFailure : CoverDownload::kInterrupted;
    };
    for (int redirect = 0; redirect <= 3 && available(); ++redirect) {
        if (!IsHttpUrl(url))
            return CoverDownload::kPermanentFailure;
        auto http = network->CreateHttp(7);
        if (!http)
            return CoverDownload::kTransientFailure;
        http->SetTimeout(3000);
        http->SetHeader("Accept-Encoding", "identity");
        if (request.source)
            request.source->ApplyHeaders(url, *http);
        if (!http->Open("GET", url)) {
            http->Close();
            return failed();
        }
        auto status = http->GetStatusCode();
        if (!status) {
            http->Close();
            return failed();
        }
        if (*status >= 300 && *status < 400) {
            const auto location = http->GetResponseHeader("Location");
            http->Close();
            if (location.empty())
                return CoverDownload::kPermanentFailure;
            url = location.rfind("//", 0) == 0 ? url.substr(0, url.find(':')) + ":" + location
                                               : ResolveUrl(url, location);
            continue;
        }
        if (*status != 200) {
            ESP_LOGW("MusicCover", "Cover HTTP status=%d, session=%lu", *status,
                     static_cast<unsigned long>(request.session));
            http->Close();
            return *status == 408 || *status == 429 || (*status >= 500 && *status <= 599)
                       ? failed()
                       : CoverDownload::kPermanentFailure;
        }
        bytes.clear();
        std::array<char, 1024> buffer;
        while (available()) {
            auto read = http->Read(buffer.data(), buffer.size());
            if (!read || *read < 0)
                break;
            if (*read == 0) {
                http->Close();
                return bytes.empty() ? CoverDownload::kPermanentFailure : CoverDownload::kReady;
            }
            if (bytes.size() + *read > kMaxCoverBytes) {
                ESP_LOGW("MusicCover", "Cover exceeds size limit, session=%lu",
                         static_cast<unsigned long>(request.session));
                http->Close();
                return CoverDownload::kPermanentFailure;
            }
            bytes.append(buffer.data(), *read);
        }
        http->Close();
        return failed();
    }
    return available() ? CoverDownload::kPermanentFailure : failed();
}

std::shared_ptr<LvglAllocatedImage> DecodeCover(const std::string& bytes) {
    size_t width = 0, height = 0, size = 0, stride = 0;
    const auto* encoded = reinterpret_cast<const uint8_t*>(bytes.data());
    if (!rlcd_cover::JpegDimensions(encoded, bytes.size(), width, height))
        return {};
    uint8_t* decoded = nullptr;
    if (jpeg_to_image(encoded, bytes.size(), &decoded, &size, &width, &height, &stride) != ESP_OK)
        return {};
    auto monochrome = rlcd_cover::Monochrome(decoded, size, width, height, stride);
    heap_caps_free(decoded);
    if (monochrome.empty())
        return {};
    auto* data = heap_caps_malloc(monochrome.size(), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data)
        return {};
    memcpy(data, monochrome.data(), monochrome.size());
    return std::make_shared<LvglAllocatedImage>(data, monochrome.size(), 128, 128, 256,
                                                LV_COLOR_FORMAT_RGB565);
}
}  // namespace

struct MusicCoverLoader::State {
    std::mutex mutex;
    std::optional<CoverRequest> pending;
    Callback callback;
    TaskHandle_t task = nullptr;
    std::atomic<bool> stopping{false};
    std::atomic<uint32_t> revision{0};
};

MusicCoverLoader::MusicCoverLoader(Callback callback) : state_(std::make_shared<State>()) {
    state_->callback = std::move(callback);
}

MusicCoverLoader::~MusicCoverLoader() {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->stopping = true;
    state_->pending.reset();
    state_->callback = {};
    if (state_->task)
        xTaskNotifyGive(state_->task);
}

void MusicCoverLoader::Request(uint32_t session, MusicTrack track,
                               std::shared_ptr<MusicSource> source, std::string root) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->stopping)
        return;
    state_->pending = CoverRequest{session, ++state_->revision, std::move(track), std::move(source),
                                   std::move(root)};
    if (!state_->task) {
        auto* ownership = new std::shared_ptr<State>(state_);
        if (xTaskCreate(Run, "music_cover", 8192, ownership, 1, &state_->task) != pdPASS) {
            delete ownership;
            state_->task = nullptr;
            state_->pending.reset();
            ESP_LOGW("MusicCover", "Cannot create cover task");
            return;
        }
    }
    xTaskNotifyGive(state_->task);
}

void MusicCoverLoader::Run(void* argument) {
    auto state = *static_cast<std::shared_ptr<State>*>(argument);
    delete static_cast<std::shared_ptr<State>*>(argument);
    while (!state->stopping) {
        std::optional<CoverRequest> request;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->pending) {
                request = std::move(state->pending);
                state->pending.reset();
            }
        }
        if (!request) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
            continue;
        }
        auto current = [&] { return !state->stopping && state->revision == request->revision; };
        // Keep speech capture/replies ahead of thumbnail downloads and decoding.
        while (current()) {
            auto mode = Application::GetInstance().GetDeviceState();
            if ((mode == kDeviceStateIdle || mode == kDeviceStatePlaying) &&
                esp_timer_get_time() >= request->retry_after)
                break;
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
        }
        if (!current())
            continue;
        MusicCache cache(request->root);
        const std::string base = request->source ? request->source->base_url() : "";
        std::string bytes;
        std::shared_ptr<MusicCache::Entry> pin;
        const bool local =
            !request->track.stream_url.empty() && request->track.stream_url[0] == '/';
        bool loaded = false;
        if (local) {
            pin = cache.PinArtwork(request->track, base);
            if (!request->track.cover_url.empty() && request->track.cover_url[0] == '/')
                loaded = ReadCover(request->track.cover_url, bytes);
            if (!loaded) {
                auto stem =
                    request->track.stream_url.substr(0, request->track.stream_url.rfind('.'));
                loaded = ReadCover(stem + ".cover.jpg", bytes) || ReadCover(stem + ".jpg", bytes);
                if (!loaded)
                    loaded = ReadCover(request->track.stream_url.substr(
                                           0, request->track.stream_url.rfind('/') + 1) +
                                           "cover.jpg",
                                       bytes);
            }
        } else if (!request->track.live && !base.empty()) {
            pin = cache.PinArtwork(request->track, base);
            loaded = ReadCover(cache.CoverPath(request->track, base), bytes);
        }
        bool from_card = loaded;
        (void)from_card;  // Logging can be compiled out.
        auto image = loaded && current() ? DecodeCover(bytes) : nullptr;
        if (!image && local) {
            // Managed audio keeps its original URL in metadata even when
            // ReadTrack points at a damaged local companion. Ordinary user
            // files deliberately have no remote artwork recovery path.
            request->track.cover_url = cache.OriginalCoverUrl(request->track);
        }
        if (!image && IsHttpUrl(request->track.cover_url)) {
            from_card = false;
            const auto result = DownloadCover(*request, bytes, [&] {
                auto mode = Application::GetInstance().GetDeviceState();
                return current() && (mode == kDeviceStateIdle || mode == kDeviceStatePlaying);
            });
            const bool retry = result == CoverDownload::kTransientFailure && request->retries < 2;
            if ((result == CoverDownload::kInterrupted || retry) && current()) {
                if (retry) {
                    ++request->retries;
                    request->retry_after = esp_timer_get_time() + request->retries * 1000000;
                    ESP_LOGW("MusicCover", "Cover retry %u/2, session=%lu", request->retries,
                             static_cast<unsigned long>(request->session));
                }
                std::lock_guard<std::mutex> lock(state->mutex);
                if (current() && !state->pending)
                    state->pending = std::move(request);
                continue;
            }
            image = result == CoverDownload::kReady && current() ? DecodeCover(bytes) : nullptr;
            if (!image && current())
                ESP_LOGW("MusicCover", "Cover unavailable, session=%lu",
                         static_cast<unsigned long>(request->session));
            // Validate completely before publishing the original song's SD
            // companion. Failed downloads leave the audio reader independent.
            if (image && (!base.empty() || local) && !request->track.live) {
                const bool saved =
                    cache.StoreCover(request->track, base, bytes.data(), bytes.size());
                ESP_LOGI("MusicCover", "Cover cache %s, session=%lu", saved ? "saved" : "skipped",
                         static_cast<unsigned long>(request->session));
                (void)saved;
            }
        }
        if (!image || !current())
            continue;
        Callback callback;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            callback = state->callback;
        }
        if (callback) {
            const auto session = request->session;
            Application::GetInstance().Schedule(
                [callback, session, image] { callback(session, image); });
            ESP_LOGI("MusicCover", "Album cover ready: session=%lu, %s",
                     static_cast<unsigned long>(session), from_card ? "SD" : "online");
#ifdef ESP_PLATFORM
            ESP_LOGI("MusicCover", "Stack remaining: %u bytes",
                     static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
#endif
        }
    }
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->task = nullptr;
    }
    state.reset();
    vTaskDelete(nullptr);
}
#endif
