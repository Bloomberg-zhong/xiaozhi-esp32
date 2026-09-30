#ifndef MUSIC_SOURCE_H_
#define MUSIC_SOURCE_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "lrc_parser.h"

class Http;

struct MusicTrack {
    std::string id;
    std::string title;
    std::string artist;
    std::string album;
    uint32_t duration_ms = 0;
    std::string stream_url;
    std::string lyric_url;   // Optional plain-text LRC URL
    std::string lyric_text;  // Optional inline LRC
    std::string provider;    // Which catalog of the server it came from
    bool live = false;       // Endless stream such as a radio station
};

// A catalog the player can search and stream from. Implementations perform
// blocking network I/O and must only be called from background tasks.
class MusicSource {
public:
    virtual ~MusicSource() = default;

    virtual const char* type() const = 0;
    virtual const std::string& base_url() const = 0;

    // An empty query asks for random or recommended songs. `provider` limits the
    // search to one catalog of a multi-catalog server; empty searches them all.
    virtual bool Search(const std::string& query, const std::string& provider, int limit,
                        std::vector<MusicTrack>& tracks, std::string& error) = 0;

    // Rebuilds a playable track from the id of an earlier search result, without
    // network access (used by favorites).
    virtual MusicTrack BuildTrack(const std::string& id, const std::string& title,
                                  const std::string& artist, bool live) const = 0;

    // Checks connectivity and credentials.
    virtual bool Ping(std::string& error) = 0;

    // Returns synchronized lyrics, or an empty list when none are available.
    virtual std::vector<LyricLine> FetchLyrics(const MusicTrack& track);

    // Adds source-specific headers (such as authorization) to requests for
    // URLs produced by this source.
    virtual void ApplyHeaders(const std::string& /*url*/, Http& /*http*/) const {}
};

// Connection ids for modem-based networks. Wi-Fi ignores them.
constexpr int kMusicStreamConnectId = 4;
constexpr int kMusicApiConnectId = 5;

// Performs a GET request and reads at most `max_bytes` of the body. Follows up
// to three redirects.
bool MusicHttpGet(const std::string& url, size_t max_bytes, std::string& body, std::string& error,
                  const MusicSource* source = nullptr);

// Persistent configuration stored in the "music" NVS namespace.
struct MusicSourceConfig {
    std::string type;  // "subsonic", "http" or "none"
    std::string url;
    std::string username;
    std::string salt;   // Subsonic token salt
    std::string token;  // Subsonic token: md5(password + salt)
    std::string api_key;
    int max_bitrate_kbps = 128;

    static MusicSourceConfig Load();
    void Save() const;
};

// Creates the source described by `config`, or nullptr when it is incomplete.
std::shared_ptr<MusicSource> CreateMusicSource(const MusicSourceConfig& config);

#endif  // MUSIC_SOURCE_H_
