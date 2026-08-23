#include "music_gateway_model.h"

#include <array>
#include <cctype>
#include <cstdio>

namespace rlcd_dashboard {
namespace {

std::string BuildSongQuery(const MusicGatewaySong& song) {
    std::string query = "id=" + UrlEncode(song.id) + "&source=" + UrlEncode(song.source);
    if (!song.name.empty()) {
        query += "&name=" + UrlEncode(song.name);
    }
    if (!song.artist.empty()) {
        query += "&artist=" + UrlEncode(song.artist);
    }
    if (!song.album.empty()) {
        query += "&album=" + UrlEncode(song.album);
    }
    if (song.duration_seconds > 0) {
        query += "&duration=" + std::to_string(song.duration_seconds);
    }
    if (!song.extra_json.empty()) {
        query += "&extra=" + UrlEncode(song.extra_json);
    }
    return query;
}

std::string BuildSongEndpoint(const std::string& base_url, const char* endpoint,
                              const MusicGatewaySong& song) {
    return NormalizeMusicGatewayBaseUrl(base_url) + endpoint + "?" + BuildSongQuery(song);
}

}  // namespace

bool MusicGatewaySong::IsValid() const {
    return !id.empty() && id.size() <= kMaxMusicSongTextBytes &&
           IsSupportedMusicGatewaySource(source) && !name.empty() &&
           name.size() <= kMaxMusicSongTextBytes && artist.size() <= kMaxMusicSongTextBytes &&
           album.size() <= kMaxMusicSongTextBytes && extra_json.size() <= kMaxMusicSongExtraBytes;
}

bool IsValidMusicGatewayBaseUrl(const std::string& value) {
    if (value.size() > kMaxMusicGatewayBaseUrlBytes) {
        return false;
    }
    const size_t prefix_length =
        value.compare(0, 7, "http://") == 0 ? 7 : (value.compare(0, 8, "https://") == 0 ? 8 : 0);
    if (prefix_length == 0 || value.size() <= prefix_length) {
        return false;
    }
    for (size_t index = prefix_length; index < value.size(); ++index) {
        const unsigned char character = static_cast<unsigned char>(value[index]);
        if (std::isspace(character) != 0 || character == '@' || character == '?' ||
            character == '#') {
            return false;
        }
    }
    return value[prefix_length] != '/';
}

bool IsValidMusicSearchQuery(const std::string& value) {
    return !value.empty() && value.size() <= kMaxMusicSearchQueryBytes;
}

bool IsSupportedMusicGatewaySource(const std::string& source) {
    static constexpr std::array<const char*, 11> kSources = {
        "netease", "qq",       "kugou", "kuwo",     "migu",    "qianqian",
        "soda",    "fivesing", "joox",  "bilibili", "jamendo",
    };
    for (const char* candidate : kSources) {
        if (source == candidate) {
            return true;
        }
    }
    return false;
}

std::string NormalizeMusicGatewayBaseUrl(const std::string& value) {
    std::string normalized = value;
    while (!normalized.empty() && normalized.back() == '/') {
        normalized.pop_back();
    }
    return normalized;
}

std::string UrlEncode(const std::string& value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size() * 3);
    for (unsigned char character : value) {
        if (std::isalnum(character) != 0 || character == '-' || character == '_' ||
            character == '.' || character == '~') {
            encoded.push_back(static_cast<char>(character));
            continue;
        }
        encoded.push_back('%');
        encoded.push_back(kHex[(character >> 4) & 0x0f]);
        encoded.push_back(kHex[character & 0x0f]);
    }
    return encoded;
}

std::string BuildMusicGatewaySearchUrl(const std::string& base_url, const std::string& query,
                                       const std::string& source) {
    std::string url = NormalizeMusicGatewayBaseUrl(base_url) +
                      "/api/v1/music/search?q=" + UrlEncode(query) + "&type=song";
    if (!source.empty() && source != "all") {
        url += "&sources=" + UrlEncode(source);
    }
    return url;
}

std::string BuildMusicGatewayStreamUrl(const std::string& base_url, const MusicGatewaySong& song) {
    return BuildSongEndpoint(base_url, "/api/v1/music/stream", song);
}

std::string BuildMusicGatewayLyricUrl(const std::string& base_url, const MusicGatewaySong& song) {
    return BuildSongEndpoint(base_url, "/music/lyric", song);
}

std::string BuildMusicGatewayInspectUrl(const std::string& base_url, const MusicGatewaySong& song) {
    return BuildSongEndpoint(base_url, "/api/v1/music/inspect", song);
}

std::string BuildMusicGatewaySwitchUrl(const std::string& base_url, const MusicGatewaySong& song) {
    std::string url = NormalizeMusicGatewayBaseUrl(base_url) +
                      "/api/v1/music/switch?name=" + UrlEncode(song.name);
    if (!song.artist.empty()) {
        url += "&artist=" + UrlEncode(song.artist);
    }
    if (!song.source.empty()) {
        url += "&source=" + UrlEncode(song.source);
    }
    if (song.duration_seconds > 0) {
        url += "&duration=" + std::to_string(song.duration_seconds);
    }
    return url;
}

}  // namespace rlcd_dashboard
