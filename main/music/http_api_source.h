#ifndef MUSIC_HTTP_API_SOURCE_H_
#define MUSIC_HTTP_API_SOURCE_H_

#include "music_source.h"

// Minimal JSON music API documented in docs/music-player.md:
//
//   GET {base}/search?q=<query>&limit=<n>
//   -> {"tracks":[{"id","title","artist","album","duration_ms","url","lyric_url","lyric"}]}
//
// scripts/music_server/music_server.py implements it for a local folder.
class HttpApiSource : public MusicSource {
public:
    HttpApiSource(std::string base_url, std::string api_key);

    const char* type() const override { return "http"; }
    const std::string& base_url() const override { return base_url_; }
    bool Search(const std::string& query, int limit, std::vector<MusicTrack>& tracks,
                std::string& error) override;
    bool Ping(std::string& error) override;
    void ApplyHeaders(const std::string& url, Http& http) const override;

private:
    std::string base_url_;
    std::string origin_;
    std::string api_key_;
};

#endif  // MUSIC_HTTP_API_SOURCE_H_
