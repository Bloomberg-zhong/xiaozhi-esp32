#include "music_gateway_client.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>

#include <cJSON.h>

#include "board.h"
#include "settings.h"

namespace rlcd_dashboard {
namespace {

constexpr const char* kSettingsNamespace = "rlcd_music";
constexpr const char* kBaseUrlKey = "gateway";
constexpr int kHttpTimeoutMs = 8000;
constexpr size_t kMaxJsonBytes = 96 * 1024;
constexpr size_t kMaxSearchResults = 12;
constexpr size_t kResultsPerSource = 3;

std::string JsonString(cJSON* object, const char* key) {
    cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsString(value) && value->valuestring != nullptr) {
        return value->valuestring;
    }
    if (cJSON_IsNumber(value) && std::isfinite(value->valuedouble)) {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.0f", value->valuedouble);
        return buffer;
    }
    return {};
}

int JsonInteger(cJSON* object, const char* key) {
    cJSON* value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsNumber(value) ? value->valueint : 0;
}

MusicGatewaySong ParseSong(cJSON* object) {
    MusicGatewaySong song;
    if (!cJSON_IsObject(object)) {
        return song;
    }
    song.id = JsonString(object, "id");
    song.source = JsonString(object, "source");
    song.name = JsonString(object, "name");
    song.artist = JsonString(object, "artist");
    song.album = JsonString(object, "album");
    song.duration_seconds = JsonInteger(object, "duration");

    cJSON* extra = cJSON_GetObjectItemCaseSensitive(object, "extra");
    if (cJSON_IsObject(extra)) {
        char* encoded = cJSON_PrintUnformatted(extra);
        if (encoded != nullptr) {
            song.extra_json = encoded;
            cJSON_free(encoded);
        }
    }
    return song;
}

const std::array<const char*, 11>& SourceOrder() {
    static constexpr std::array<const char*, 11> kOrder = {
        "netease",  "qq",      "kugou", "kuwo",     "migu", "qianqian",
        "bilibili", "jamendo", "joox",  "fivesing", "soda",
    };
    return kOrder;
}

std::vector<MusicGatewaySong> SelectSearchResults(std::vector<MusicGatewaySong> songs,
                                                  const std::string& requested_source) {
    std::vector<MusicGatewaySong> selected;
    selected.reserve(std::min(kMaxSearchResults, songs.size()));
    if (requested_source != "all") {
        for (auto& song : songs) {
            if (selected.size() >= kMaxSearchResults) {
                break;
            }
            selected.push_back(std::move(song));
        }
        return selected;
    }

    for (const char* source : SourceOrder()) {
        size_t source_count = 0;
        for (auto& song : songs) {
            if (selected.size() >= kMaxSearchResults || source_count >= kResultsPerSource) {
                break;
            }
            if (song.source == source) {
                selected.push_back(std::move(song));
                ++source_count;
            }
        }
        if (selected.size() >= kMaxSearchResults) {
            break;
        }
    }
    return selected;
}

}  // namespace

MusicGatewayClient& MusicGatewayClient::Instance() {
    static MusicGatewayClient instance;
    return instance;
}

MusicGatewayClient::MusicGatewayClient() {
    Settings settings(kSettingsNamespace, false);
    const std::string saved = settings.GetString(kBaseUrlKey);
    if (IsValidMusicGatewayBaseUrl(saved)) {
        base_url_ = NormalizeMusicGatewayBaseUrl(saved);
    }
}

bool MusicGatewayClient::Configure(const std::string& base_url, std::string& error) {
    if (!IsValidMusicGatewayBaseUrl(base_url)) {
        error = "网关地址必须是有效的 HTTP/HTTPS URL";
        return false;
    }
    const std::string normalized = NormalizeMusicGatewayBaseUrl(base_url);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        base_url_ = normalized;
        search_results_.clear();
    }
    Settings settings(kSettingsNamespace, true);
    settings.SetString(kBaseUrlKey, normalized);
    return true;
}

std::string MusicGatewayClient::GetBaseUrl() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return base_url_;
}

std::vector<MusicGatewaySong> MusicGatewayClient::Search(const std::string& query,
                                                         const std::string& source,
                                                         std::string& error) {
    if (query.empty()) {
        error = "搜索关键词不能为空";
        return {};
    }
    const std::string selected_source = source.empty() ? "all" : source;
    if (selected_source != "all" && !IsSupportedMusicGatewaySource(selected_source)) {
        error = "不支持的音乐源";
        return {};
    }

    const std::string base_url = GetBaseUrl();
    if (base_url.empty()) {
        error = "尚未配置音乐网关，请先调用 self.music.gateway.configure";
        return {};
    }

    std::string body;
    if (!GetJson(BuildMusicGatewaySearchUrl(base_url, query, selected_source), body, error)) {
        return {};
    }

    cJSON* root = cJSON_ParseWithLength(body.c_str(), body.size());
    cJSON* code = root == nullptr ? nullptr : cJSON_GetObjectItemCaseSensitive(root, "code");
    cJSON* data = root == nullptr ? nullptr : cJSON_GetObjectItemCaseSensitive(root, "data");
    cJSON* songs_node = data == nullptr ? nullptr : cJSON_GetObjectItemCaseSensitive(data, "songs");
    if (root == nullptr || !cJSON_IsNumber(code) || code->valueint != 200 ||
        !cJSON_IsArray(songs_node)) {
        cJSON_Delete(root);
        error = "音乐网关返回了无效的搜索响应";
        return {};
    }

    std::vector<MusicGatewaySong> songs;
    cJSON* entry = nullptr;
    cJSON_ArrayForEach (entry, songs_node) {
        MusicGatewaySong song = ParseSong(entry);
        if (song.IsValid()) {
            songs.push_back(std::move(song));
        }
    }
    cJSON_Delete(root);

    songs = SelectSearchResults(std::move(songs), selected_source);
    if (songs.empty()) {
        error = "没有搜索到可识别的歌曲";
        return {};
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        search_results_ = songs;
    }
    return songs;
}

std::optional<MusicGatewayPlayback> MusicGatewayClient::ResolvePlayback(size_t one_based_index,
                                                                        std::string& error) {
    MusicGatewaySong song;
    std::string base_url;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (one_based_index == 0 || one_based_index > search_results_.size()) {
            error = "歌曲序号无效，请先调用 self.music.search";
            return std::nullopt;
        }
        song = search_results_[one_based_index - 1];
        base_url = base_url_;
    }

    bool playable = false;
    std::string inspect_error;
    const bool inspected = InspectPlayable(base_url, song, playable, inspect_error);
    bool used_fallback = false;
    if (!inspected || !playable) {
        auto switched = SwitchSource(base_url, song, error);
        if (!switched.has_value()) {
            if (!inspected && !inspect_error.empty()) {
                error = inspect_error + "；自动换源也失败：" + error;
            }
            return std::nullopt;
        }
        song = std::move(*switched);
        used_fallback = true;
    }

    MusicGatewayPlayback playback;
    playback.song = song;
    playback.audio_url = BuildMusicGatewayStreamUrl(base_url, song);
    playback.lyric_url = BuildMusicGatewayLyricUrl(base_url, song);
    playback.used_fallback = used_fallback;
    return playback;
}

bool MusicGatewayClient::GetJson(const std::string& url, std::string& body,
                                 std::string& error) const {
    auto* network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        error = "网络接口不可用";
        return false;
    }
    auto http = network->CreateHttp(3);
    if (!http) {
        error = "无法创建 HTTP 请求";
        return false;
    }
    http->SetTimeout(kHttpTimeoutMs);
    http->SetHeader("Accept", "application/json");
    http->SetHeader("Accept-Encoding", "identity");
    if (!http->Open("GET", url)) {
        error = "无法连接音乐网关";
        http->Close();
        return false;
    }
    const int status = http->GetStatusCode();
    if (status < 200 || status >= 300) {
        error = "音乐网关请求失败，HTTP " + std::to_string(status);
        http->Close();
        return false;
    }
    const size_t declared_size = http->GetBodyLength();
    if (declared_size > kMaxJsonBytes) {
        error = "音乐网关响应过大";
        http->Close();
        return false;
    }

    body.clear();
    if (declared_size > 0) {
        body.reserve(declared_size);
    }
    std::array<char, 1024> buffer;
    while (body.size() < kMaxJsonBytes) {
        const size_t remaining = kMaxJsonBytes - body.size();
        const int size = http->Read(buffer.data(), std::min(buffer.size(), remaining));
        if (size < 0) {
            error = "读取音乐网关响应失败";
            http->Close();
            return false;
        }
        if (size == 0) {
            break;
        }
        body.append(buffer.data(), static_cast<size_t>(size));
    }
    if (body.size() == kMaxJsonBytes) {
        error = "音乐网关响应超过大小限制";
        http->Close();
        return false;
    }
    http->Close();
    return true;
}

bool MusicGatewayClient::InspectPlayable(const std::string& base_url, const MusicGatewaySong& song,
                                         bool& playable, std::string& error) const {
    std::string body;
    if (!GetJson(BuildMusicGatewayInspectUrl(base_url, song), body, error)) {
        return false;
    }
    cJSON* root = cJSON_ParseWithLength(body.c_str(), body.size());
    cJSON* valid = root == nullptr ? nullptr : cJSON_GetObjectItemCaseSensitive(root, "valid");
    if (!cJSON_IsBool(valid)) {
        cJSON_Delete(root);
        error = "音乐网关返回了无效的探测响应";
        return false;
    }
    playable = cJSON_IsTrue(valid);
    cJSON_Delete(root);
    return true;
}

std::optional<MusicGatewaySong> MusicGatewayClient::SwitchSource(const std::string& base_url,
                                                                 const MusicGatewaySong& song,
                                                                 std::string& error) const {
    std::string body;
    if (!GetJson(BuildMusicGatewaySwitchUrl(base_url, song), body, error)) {
        return std::nullopt;
    }
    cJSON* root = cJSON_ParseWithLength(body.c_str(), body.size());
    MusicGatewaySong switched = ParseSong(root);
    cJSON_Delete(root);
    if (!switched.IsValid()) {
        error = "音乐网关未找到可播放的替代音源";
        return std::nullopt;
    }
    return switched;
}

}  // namespace rlcd_dashboard
