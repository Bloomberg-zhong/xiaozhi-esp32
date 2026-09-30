#include "music_source.h"

#include <esp_log.h>
#include <esp_random.h>

#include <cJSON.h>

#include <array>
#include <memory>

#include "board.h"
#include "http_api_source.h"
#include "music_util.h"
#include "settings.h"
#include "subsonic_source.h"

#define TAG "MusicSource"

namespace {
constexpr int kHttpTimeoutMs = 8000;
constexpr int kMaxRedirects = 3;
constexpr size_t kMaxLyricBytes = 64 * 1024;
constexpr const char* kSettingsNamespace = "music";
}  // namespace

std::vector<LyricLine> MusicSource::FetchLyrics(const MusicTrack& track) {
    if (!track.lyric_text.empty()) {
        return ParseLrc(track.lyric_text);
    }
    if (track.lyric_url.empty()) {
        return {};
    }
    std::string body;
    std::string error;
    if (!MusicHttpGet(track.lyric_url, kMaxLyricBytes, body, error, this)) {
        ESP_LOGW(TAG, "Failed to download lyrics: %s", error.c_str());
        return {};
    }
    return ParseLrc(body);
}

bool MusicHttpGet(const std::string& url, size_t max_bytes, std::string& body, std::string& error,
                  const MusicSource* source) {
    auto network = Board::GetInstance().GetNetwork();
    std::string current = url;
    for (int redirect = 0; redirect <= kMaxRedirects; ++redirect) {
        if (!IsHttpUrl(current)) {
            error = "invalid URL";
            return false;
        }
        auto http = network->CreateHttp(kMusicApiConnectId);
        if (!http) {
            error = "network unavailable";
            return false;
        }
        http->SetTimeout(kHttpTimeoutMs);
        http->SetHeader("Accept-Encoding", "identity");
        if (source != nullptr) {
            source->ApplyHeaders(current, *http);
        }
        if (!http->Open("GET", current)) {
            error = "cannot connect to the music server";
            return false;
        }
        auto status_result = http->GetStatusCode();
        if (!status_result) {
            http->Close();
            error = "cannot read the music server response";
            return false;
        }
        const int status = *status_result;
        if (status >= 300 && status < 400) {
            std::string location = http->GetResponseHeader("Location");
            http->Close();
            if (location.empty()) {
                error = "redirect without location";
                return false;
            }
            current = ResolveUrl(current, location);
            continue;
        }
        if (status < 200 || status >= 300) {
            error = "music server returned HTTP " + std::to_string(status);
            // Servers explain failures as {"error": "..."}; pass that on so the
            // assistant can react (for example to an unknown source name).
            std::array<char, 384> detail;
            auto read = http->Read(detail.data(), detail.size() - 1);
            if (read && *read > 0) {
                detail[*read] = '\0';
                std::unique_ptr<cJSON, decltype(&cJSON_Delete)> json(cJSON_Parse(detail.data()),
                                                                     cJSON_Delete);
                const cJSON* message = json ? cJSON_GetObjectItem(json.get(), "error") : nullptr;
                if (cJSON_IsString(message)) {
                    error += ": ";
                    error += message->valuestring;
                }
            }
            http->Close();
            return false;
        }

        body.clear();
        std::array<char, 1024> buffer;
        while (true) {
            auto read = http->Read(buffer.data(), buffer.size());
            int size = read ? *read : -1;
            if (size < 0) {
                http->Close();
                error = "failed to read the response";
                return false;
            }
            if (size == 0) {
                break;
            }
            if (body.size() + size > max_bytes) {
                http->Close();
                error = "response is too large";
                return false;
            }
            body.append(buffer.data(), size);
        }
        http->Close();
        return true;
    }
    error = "too many redirects";
    return false;
}

MusicSourceConfig MusicSourceConfig::Load() {
    MusicSourceConfig config;
    Settings settings(kSettingsNamespace, false);
    config.type = settings.GetString("type");
    if (!config.type.empty()) {
        config.url = settings.GetString("url");
        config.username = settings.GetString("user");
        config.salt = settings.GetString("salt");
        config.token = settings.GetString("token");
        config.api_key = settings.GetString("api_key");
        config.max_bitrate_kbps = settings.GetInt("bitrate", 128);
        return config;
    }

    // Nothing stored yet: use the defaults from menuconfig. The music server
    // (docs/music-player.md) takes precedence over a direct Subsonic account.
#if defined(CONFIG_MUSIC_SERVER_URL)
    if (std::string(CONFIG_MUSIC_SERVER_URL) != "") {
        config.type = "http";
        config.url = CONFIG_MUSIC_SERVER_URL;
        config.api_key = CONFIG_MUSIC_SERVER_API_KEY;
        return config;
    }
#endif
#if defined(CONFIG_MUSIC_SOURCE_DEFAULT_SUBSONIC)
    config.type = "subsonic";
    config.url = CONFIG_MUSIC_SOURCE_DEFAULT_URL;
    config.username = CONFIG_MUSIC_SOURCE_DEFAULT_USERNAME;
    std::string password = CONFIG_MUSIC_SOURCE_DEFAULT_PASSWORD;
    if (!password.empty()) {
        std::array<uint8_t, 8> salt;
        esp_fill_random(salt.data(), salt.size());
        config.salt = HexEncode(salt.data(), salt.size());
        config.token = SubsonicSource::MakeToken(password, config.salt);
    }
#endif
    return config;
}

void MusicSourceConfig::Save() const {
    // "none" is stored explicitly so that menuconfig defaults stop applying.
    Settings settings(kSettingsNamespace, true);
    settings.SetString("type", type.empty() ? "none" : type);
    settings.SetString("url", url);
    settings.SetString("user", username);
    settings.SetString("salt", salt);
    settings.SetString("token", token);
    settings.SetString("api_key", api_key);
    settings.SetInt("bitrate", max_bitrate_kbps);
}

std::shared_ptr<MusicSource> CreateMusicSource(const MusicSourceConfig& config) {
    if (!IsHttpUrl(config.url)) {
        return nullptr;
    }
    if (config.type == "subsonic") {
        if (config.username.empty() || config.token.empty() || config.salt.empty()) {
            return nullptr;
        }
        return std::make_shared<SubsonicSource>(config.url, config.username, config.salt,
                                                config.token, config.max_bitrate_kbps);
    }
    if (config.type == "http") {
        return std::make_shared<HttpApiSource>(config.url, config.api_key);
    }
    return nullptr;
}
