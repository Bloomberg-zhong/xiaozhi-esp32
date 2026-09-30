#include "subsonic_source.h"

#include <esp_log.h>
#include <esp_rom_md5.h>
#include <cJSON.h>

#include <algorithm>
#include <memory>

#include "music_util.h"

#define TAG "Subsonic"

namespace {
constexpr const char* kApiVersion = "1.16.1";
constexpr const char* kClientName = "xiaozhi";
constexpr size_t kMaxResponseBytes = 64 * 1024;

using JsonPtr = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

std::string JsonString(const cJSON* object, const char* key) {
    const cJSON* item = cJSON_GetObjectItem(object, key);
    if (cJSON_IsString(item)) {
        return item->valuestring;
    }
    if (cJSON_IsNumber(item)) {
        return std::to_string(static_cast<long long>(item->valuedouble));
    }
    return "";
}

// Returns the "subsonic-response" object, or nullptr with `error` set.
const cJSON* GetResponse(const cJSON* root, std::string& error) {
    const cJSON* response = cJSON_GetObjectItem(root, "subsonic-response");
    if (!cJSON_IsObject(response)) {
        error = "not a Subsonic server response";
        return nullptr;
    }
    const cJSON* status = cJSON_GetObjectItem(response, "status");
    if (!cJSON_IsString(status) || std::string(status->valuestring) != "ok") {
        const cJSON* failure = cJSON_GetObjectItem(response, "error");
        std::string message = JsonString(failure, "message");
        error = message.empty() ? "Subsonic request failed" : message;
        return nullptr;
    }
    return response;
}

}  // namespace

SubsonicSource::SubsonicSource(std::string base_url, std::string username, std::string salt,
                               std::string token, int max_bitrate_kbps)
    : base_url_(std::move(base_url)),
      username_(std::move(username)),
      salt_(std::move(salt)),
      token_(std::move(token)),
      max_bitrate_kbps_(max_bitrate_kbps) {}

std::string SubsonicSource::MakeToken(const std::string& password, const std::string& salt) {
    md5_context_t context;
    uint8_t digest[16];
    esp_rom_md5_init(&context);
    esp_rom_md5_update(&context, password.data(), password.size());
    esp_rom_md5_update(&context, salt.data(), salt.size());
    esp_rom_md5_final(digest, &context);
    return HexEncode(digest, sizeof(digest));
}

std::string SubsonicSource::ApiUrl(const std::string& method,
                                   std::vector<std::pair<std::string, std::string>> params) const {
    params.insert(params.begin(), {{"u", username_},
                                   {"t", token_},
                                   {"s", salt_},
                                   {"v", kApiVersion},
                                   {"c", kClientName},
                                   {"f", "json"}});
    return BuildUrl(base_url_, "rest/" + method, params);
}

bool SubsonicSource::Request(const std::string& method,
                             std::vector<std::pair<std::string, std::string>> params,
                             size_t max_bytes, std::string& body, std::string& error) const {
    return MusicHttpGet(ApiUrl(method, std::move(params)), max_bytes, body, error, this);
}

bool SubsonicSource::Ping(std::string& error) {
    std::string body;
    if (!Request("ping.view", {}, 4096, body, error)) {
        return false;
    }
    JsonPtr root(cJSON_Parse(body.c_str()), cJSON_Delete);
    return root && GetResponse(root.get(), error) != nullptr;
}

bool SubsonicSource::ParseSong(const cJSON* song, MusicTrack& track) const {
    track.id = JsonString(song, "id");
    track.title = JsonString(song, "title");
    if (track.id.empty() || track.title.empty()) {
        return false;
    }
    track.artist = JsonString(song, "artist");
    track.album = JsonString(song, "album");
    track.provider = "subsonic";
    const cJSON* duration = cJSON_GetObjectItem(song, "duration");
    if (cJSON_IsNumber(duration) && duration->valuedouble > 0) {
        track.duration_ms = static_cast<uint32_t>(duration->valuedouble * 1000);
    }
    track.stream_url = StreamUrl(track.id);
    return true;
}

std::string SubsonicSource::StreamUrl(const std::string& id) const {
    std::vector<std::pair<std::string, std::string>> stream_params = {{"id", id}};
    if (max_bitrate_kbps_ > 0) {
        // Ask the server to transcode everything to MP3, which every
        // supported device can decode with little memory.
        stream_params.push_back({"format", "mp3"});
        stream_params.push_back({"maxBitRate", std::to_string(max_bitrate_kbps_)});
    }
    return ApiUrl("stream.view", stream_params);
}

MusicTrack SubsonicSource::BuildTrack(const std::string& id, const std::string& title,
                                      const std::string& artist, bool) const {
    MusicTrack track;
    track.id = id;
    track.title = title;
    track.artist = artist;
    track.provider = "subsonic";
    track.stream_url = StreamUrl(id);
    return track;
}

bool SubsonicSource::Search(const std::string& query, const std::string& provider, int limit,
                            std::vector<MusicTrack>& tracks, std::string& error) {
    if (!provider.empty() && provider != "subsonic") {
        error = "this music source only has the catalog subsonic";
        return false;
    }
    limit = std::clamp(limit, 1, 50);
    std::string body;
    const bool random = query.empty();
    bool ok = random ? Request("getRandomSongs.view", {{"size", std::to_string(limit)}},
                               kMaxResponseBytes, body, error)
                     : Request("search3.view",
                               {{"query", query},
                                {"songCount", std::to_string(limit)},
                                {"artistCount", "0"},
                                {"albumCount", "0"}},
                               kMaxResponseBytes, body, error);
    if (!ok) {
        return false;
    }

    JsonPtr root(cJSON_Parse(body.c_str()), cJSON_Delete);
    if (!root) {
        error = "invalid JSON from the music server";
        return false;
    }
    const cJSON* response = GetResponse(root.get(), error);
    if (response == nullptr) {
        return false;
    }
    const cJSON* result = cJSON_GetObjectItem(response, random ? "randomSongs" : "searchResult3");
    const cJSON* songs = cJSON_GetObjectItem(result, "song");

    tracks.clear();
    const cJSON* song = nullptr;
    cJSON_ArrayForEach (song, songs) {
        MusicTrack track;
        if (ParseSong(song, track)) {
            tracks.push_back(std::move(track));
        }
        if (static_cast<int>(tracks.size()) >= limit) {
            break;
        }
    }
    if (tracks.empty()) {
        error = random ? "the music library is empty" : "no matching songs";
        return false;
    }
    return true;
}

std::vector<LyricLine> SubsonicSource::FetchLyrics(const MusicTrack& track) {
    std::string body;
    std::string error;

    // OpenSubsonic extension with synchronized lyrics.
    if (Request("getLyricsBySongId.view", {{"id", track.id}}, kMaxResponseBytes, body, error)) {
        JsonPtr root(cJSON_Parse(body.c_str()), cJSON_Delete);
        const cJSON* response = root ? GetResponse(root.get(), error) : nullptr;
        const cJSON* list = cJSON_GetObjectItem(response, "lyricsList");
        const cJSON* structured = cJSON_GetObjectItem(list, "structuredLyrics");
        const cJSON* lyrics = nullptr;
        cJSON_ArrayForEach (lyrics, structured) {
            if (!cJSON_IsTrue(cJSON_GetObjectItem(lyrics, "synced"))) {
                continue;
            }
            int offset_ms = 0;
            const cJSON* offset = cJSON_GetObjectItem(lyrics, "offset");
            if (cJSON_IsNumber(offset)) {
                offset_ms = offset->valueint;
            }
            std::vector<LyricLine> lines;
            const cJSON* line = nullptr;
            cJSON_ArrayForEach (line, cJSON_GetObjectItem(lyrics, "line")) {
                const cJSON* start = cJSON_GetObjectItem(line, "start");
                const cJSON* value = cJSON_GetObjectItem(line, "value");
                if (!cJSON_IsNumber(start) || !cJSON_IsString(value) || lines.size() >= 512) {
                    continue;
                }
                double time_ms = start->valuedouble - offset_ms;
                lines.push_back(
                    {static_cast<uint32_t>(std::max(time_ms, 0.0)), value->valuestring});
            }
            if (!lines.empty()) {
                std::stable_sort(
                    lines.begin(), lines.end(),
                    [](const LyricLine& a, const LyricLine& b) { return a.time_ms < b.time_ms; });
                return lines;
            }
        }
    }

    // Classic endpoint: plain text that is often LRC formatted.
    if (track.artist.empty() && track.title.empty()) {
        return {};
    }
    if (!Request("getLyrics.view", {{"artist", track.artist}, {"title", track.title}},
                 kMaxResponseBytes, body, error)) {
        ESP_LOGD(TAG, "No lyrics: %s", error.c_str());
        return {};
    }
    JsonPtr root(cJSON_Parse(body.c_str()), cJSON_Delete);
    const cJSON* response = root ? GetResponse(root.get(), error) : nullptr;
    std::string text = JsonString(cJSON_GetObjectItem(response, "lyrics"), "value");
    return ParseLrc(text);
}
