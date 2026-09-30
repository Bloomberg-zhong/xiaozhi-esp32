#include "http_api_source.h"

#include <cJSON.h>

#include <algorithm>
#include <memory>

#include "http.h"
#include "music_util.h"

namespace {
constexpr size_t kMaxResponseBytes = 64 * 1024;
constexpr size_t kMaxInlineLyricBytes = 32 * 1024;

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
}  // namespace

HttpApiSource::HttpApiSource(std::string base_url, std::string api_key)
    : base_url_(std::move(base_url)), origin_(UrlOrigin(base_url_)), api_key_(std::move(api_key)) {}

void HttpApiSource::ApplyHeaders(const std::string& url, Http& http) const {
    // Only send the key to the configured server, never to third-party URLs.
    if (!api_key_.empty() && UrlOrigin(url) == origin_) {
        http.SetHeader("Authorization", "Bearer " + api_key_);
    }
}

bool HttpApiSource::Ping(std::string& error) {
    // The health check is public; a search proves the API key as well.
    std::vector<MusicTrack> tracks;
    if (Search("", "", 1, tracks, error)) {
        return true;
    }
    // An empty library still proves that the server and key work.
    return error == "the music library is empty";
}

bool HttpApiSource::ParseTrack(const cJSON* item, MusicTrack& track) const {
    track.id = JsonString(item, "id");
    track.title = JsonString(item, "title");
    track.stream_url = ResolveUrl(base_url_, JsonString(item, "url"));
    if (track.title.empty() || !IsHttpUrl(track.stream_url)) {
        return false;
    }
    track.artist = JsonString(item, "artist");
    track.album = JsonString(item, "album");
    track.provider = JsonString(item, "source");
    track.live = cJSON_IsTrue(cJSON_GetObjectItem(item, "live"));
    const cJSON* duration = cJSON_GetObjectItem(item, "duration_ms");
    if (cJSON_IsNumber(duration) && duration->valuedouble > 0) {
        track.duration_ms = static_cast<uint32_t>(duration->valuedouble);
    }
    std::string lyric_url = JsonString(item, "lyric_url");
    if (!lyric_url.empty()) {
        track.lyric_url = ResolveUrl(base_url_, lyric_url);
        if (!IsHttpUrl(track.lyric_url)) {
            track.lyric_url.clear();
        }
    }
    track.lyric_text = JsonString(item, "lyric");
    if (track.lyric_text.size() > kMaxInlineLyricBytes) {
        track.lyric_text.clear();
    }
    return true;
}

bool HttpApiSource::Search(const std::string& query, const std::string& provider, int limit,
                           std::vector<MusicTrack>& tracks, std::string& error) {
    limit = std::clamp(limit, 1, 50);
    std::string body;
    std::vector<std::pair<std::string, std::string>> params = {{"q", query},
                                                               {"limit", std::to_string(limit)}};
    if (!provider.empty()) {
        params.push_back({"source", provider});
    }
    if (!MusicHttpGet(BuildUrl(base_url_, "search", params), kMaxResponseBytes, body, error,
                      this)) {
        return false;
    }

    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_Parse(body.c_str()), cJSON_Delete);
    const cJSON* list = root ? cJSON_GetObjectItem(root.get(), "tracks") : nullptr;
    if (!cJSON_IsArray(list)) {
        error = "invalid response from the music server";
        return false;
    }

    tracks.clear();
    const cJSON* item = nullptr;
    cJSON_ArrayForEach (item, list) {
        MusicTrack track;
        if (ParseTrack(item, track)) {
            tracks.push_back(std::move(track));
        }
        if (static_cast<int>(tracks.size()) >= limit) {
            break;
        }
    }
    if (tracks.empty()) {
        error = query.empty() ? "the music library is empty" : "no matching songs";
        return false;
    }
    return true;
}

MusicTrack HttpApiSource::BuildTrack(const std::string& id, const std::string& title,
                                     const std::string& artist, bool live) const {
    MusicTrack track;
    track.id = id;
    track.title = title;
    track.artist = artist;
    track.live = live;
    track.stream_url = BuildUrl(base_url_, "stream/" + UrlEncode(id), {});
    if (!live) {
        track.lyric_url = BuildUrl(base_url_, "lyrics/" + UrlEncode(id), {});
    }
    return track;
}
