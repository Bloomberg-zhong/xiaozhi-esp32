#pragma once

#include <string>

namespace rlcd_dashboard {

inline constexpr size_t kMaxMusicGatewayBaseUrlBytes = 256;
inline constexpr size_t kMaxMusicSearchQueryBytes = 128;
inline constexpr size_t kMaxMusicSongTextBytes = 192;
inline constexpr size_t kMaxMusicSongExtraBytes = 2048;

struct MusicGatewaySong {
    std::string id;
    std::string source;
    std::string name;
    std::string artist;
    std::string album;
    std::string extra_json;
    int duration_seconds = 0;

    bool IsValid() const;
};

bool IsValidMusicGatewayBaseUrl(const std::string& value);
bool IsValidMusicSearchQuery(const std::string& value);
bool IsSupportedMusicGatewaySource(const std::string& source);
std::string NormalizeMusicGatewayBaseUrl(const std::string& value);
std::string UrlEncode(const std::string& value);

std::string BuildMusicGatewaySearchUrl(const std::string& base_url, const std::string& query,
                                       const std::string& source);
std::string BuildMusicGatewayStreamUrl(const std::string& base_url, const MusicGatewaySong& song);
std::string BuildMusicGatewayLyricUrl(const std::string& base_url, const MusicGatewaySong& song);
std::string BuildMusicGatewayInspectUrl(const std::string& base_url, const MusicGatewaySong& song);
std::string BuildMusicGatewaySwitchUrl(const std::string& base_url, const MusicGatewaySong& song);

}  // namespace rlcd_dashboard
