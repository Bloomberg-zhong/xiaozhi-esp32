#ifndef MUSIC_UTIL_H_
#define MUSIC_UTIL_H_

// Pure helpers without ESP-IDF dependencies so they can be unit tested on the
// host (see scripts/tests/test_music_player_logic.py).

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

enum class MusicAudioFormat {
    kUnknown,
    kMp3,
    kAac,  // ADTS
    kM4a,
    kFlac,
    kWav,
};

const char* MusicAudioFormatName(MusicAudioFormat format);

// Detects the container from the first bytes of the stream. Falls back to the
// Content-Type header and then to the URL path extension.
MusicAudioFormat DetectMusicAudioFormat(const uint8_t* data, size_t size,
                                        const std::string& content_type, const std::string& url);

bool IsHttpUrl(const std::string& url);

// Percent-encodes everything except RFC 3986 unreserved characters.
std::string UrlEncode(const std::string& value);

// Builds "base/path?key=value&..." with encoded values. A trailing '/' on
// `base` and a leading '/' on `path` are merged.
std::string BuildUrl(const std::string& base, const std::string& path,
                     const std::vector<std::pair<std::string, std::string>>& query);

// Resolves `reference` against `base`: absolute http(s) URLs are returned as
// is, "/path" is resolved against the origin of `base`, anything else against
// the base path.
std::string ResolveUrl(const std::string& base, const std::string& reference);

// Returns "scheme://host[:port]" of an http(s) URL, or an empty string.
std::string UrlOrigin(const std::string& url);

// Converts interleaved little-endian PCM (16, 24 or 32 bits per sample) to
// 16-bit mono by averaging the channels, appending the result to `out`.
// Returns the number of frames converted; a trailing partial frame is ignored.
size_t DownmixToMono16(const uint8_t* data, size_t bytes, int channels, int bits_per_sample,
                       std::vector<int16_t>& out);

// Lower-case hexadecimal encoding.
std::string HexEncode(const uint8_t* data, size_t size);

#endif  // MUSIC_UTIL_H_
