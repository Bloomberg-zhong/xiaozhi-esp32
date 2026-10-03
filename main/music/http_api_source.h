#ifndef MUSIC_HTTP_API_SOURCE_H_
#define MUSIC_HTTP_API_SOURCE_H_

#include <cJSON.h>

#include "music_source.h"

// JSON music API documented in docs/music-player.md:
//
//   GET {base}/search?q=<query>&limit=<n>[&source=<catalog>]
//   -> {"tracks":[{"id","title","artist","album","duration_ms","url","lyric_url",
//                  "cover_url","lyric","source","live"}]}
//   GET {base}/stream/<id>    audio of a song, `id` percent-encoded
//   GET {base}/lyrics/<id>    LRC text, 404 without lyrics
//   GET {base}/cover/<id>?size=128  bounded baseline JPEG, 404 without artwork
//
// scripts/music_server implements it on top of several free catalogs.
class HttpApiSource : public MusicSource {
public:
    HttpApiSource(std::string base_url, std::string api_key);

    const char* type() const override { return "http"; }
    const std::string& base_url() const override { return base_url_; }
    bool Search(const std::string& query, const std::string& provider, int limit,
                std::vector<MusicTrack>& tracks, std::string& error) override;
    MusicTrack BuildTrack(const std::string& id, const std::string& title,
                          const std::string& artist, bool live) const override;
    bool Ping(std::string& error) override;
    void ApplyHeaders(const std::string& url, Http& http) const override;

private:
    bool ParseTrack(const cJSON* item, MusicTrack& track) const;

    std::string base_url_;
    std::string origin_;
    std::string api_key_;
};

#endif  // MUSIC_HTTP_API_SOURCE_H_
