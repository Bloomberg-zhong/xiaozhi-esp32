#ifndef MUSIC_SUBSONIC_SOURCE_H_
#define MUSIC_SUBSONIC_SOURCE_H_

#include "music_source.h"

// Subsonic / OpenSubsonic API client (Navidrome, Gonic, Airsonic, ...).
// Only the salted token (md5(password + salt)) is kept, never the password.
class SubsonicSource : public MusicSource {
public:
    SubsonicSource(std::string base_url, std::string username, std::string salt, std::string token,
                   int max_bitrate_kbps);

    static std::string MakeToken(const std::string& password, const std::string& salt);

    const char* type() const override { return "subsonic"; }
    const std::string& base_url() const override { return base_url_; }
    bool Search(const std::string& query, int limit, std::vector<MusicTrack>& tracks,
                std::string& error) override;
    bool Ping(std::string& error) override;
    std::vector<LyricLine> FetchLyrics(const MusicTrack& track) override;

private:
    std::string base_url_;
    std::string username_;
    std::string salt_;
    std::string token_;
    int max_bitrate_kbps_;

    std::string ApiUrl(const std::string& method,
                       std::vector<std::pair<std::string, std::string>> params) const;
    bool Request(const std::string& method, std::vector<std::pair<std::string, std::string>> params,
                 size_t max_bytes, std::string& body, std::string& error) const;
};

#endif  // MUSIC_SUBSONIC_SOURCE_H_
