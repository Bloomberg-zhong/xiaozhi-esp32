"""Compile the actual HTTP/Subsonic sources against host I/O stubs."""

import os
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
MUSIC = ROOT / "main" / "music"
CJSON = ROOT / "managed_components" / "espressif__cjson" / "cJSON"


class SourceArtworkTests(unittest.TestCase):
    def test_source_metadata_and_saved_track_cover_addresses(self):
        if not (CJSON / "cJSON.c").exists():
            self.skipTest("cJSON dependency is not available")
        with tempfile.TemporaryDirectory() as directory:
            folder = pathlib.Path(directory)
            (folder / "http.h").write_text('''#pragma once
#include <string>
class Http { public: std::string authorization;
void SetHeader(const std::string&, const std::string& value) { authorization = value; } };
''')
            (folder / "esp_log.h").write_text('#pragma once\n#define ESP_LOGD(...) ((void)0)\n')
            (folder / "esp_rom_md5.h").write_text('''#pragma once
#include <cstddef>
struct md5_context_t {};
inline void esp_rom_md5_init(md5_context_t*) {}
inline void esp_rom_md5_update(md5_context_t*, const void*, size_t) {}
inline void esp_rom_md5_final(unsigned char* digest, md5_context_t*) {
for (int i=0; i<16; ++i) digest[i] = 0; }
''')
            driver = folder / "driver.cc"
            driver.write_text(r'''
#include <cassert>
#include <utility>
#include "http_api_source.h"
#include "music_cache.h"
#include "subsonic_source.h"
#include "http.h"
std::string response_body;
bool MusicHttpGet(const std::string&, size_t, std::string& body, std::string&,
                  const MusicSource*) { body = response_body; return true; }
std::vector<LyricLine> MusicSource::FetchLyrics(const MusicTrack&) { return {}; }
int main() {
    HttpApiSource http("https://music.example/api", "key");
    auto saved = http.BuildTrack("gateway:qq:1/a", "song", "artist", false);
    assert(saved.cover_url == "https://music.example/api/cover/gateway%3Aqq%3A1%2Fa?size=128");
    std::vector<MusicTrack> tracks;
    std::string error;
    MusicCache cache("/sdcard");
    for (const auto& item : {std::pair<const char*, const char*>{"gateway:kuwo:440613", "gateway"},
                            {"local:children/Artist - Song.mp3", "local"},
                            {"jamendo:440613", "jamendo"}}) {
        response_body = std::string(R"({"tracks":[{"id":")") + item.first +
                        R"(","title":"song","artist":"artist","url":"/stream/1","source":")" +
                        item.second + R"("}]})";
        assert(http.Search("song", "", 1, tracks, error));
        assert(tracks[0].provider == item.second);
        auto favorite = http.BuildTrack(tracks[0].id, tracks[0].title, tracks[0].artist, false);
        assert(favorite.provider == tracks[0].provider);
        // Search and favorites must address the same existing audio/artwork cache identity.
        assert(cache.CoverPath(favorite, http.base_url()) ==
               cache.CoverPath(tracks[0], http.base_url()));
    }
    assert(saved.provider == "gateway");  // Nested gateway catalog remains part of the id.
    auto other_catalog = http.BuildTrack("gateway:qq:440613", "song", "artist", false);
    auto first_catalog = http.BuildTrack("gateway:kuwo:440613", "song", "artist", false);
    assert(cache.CoverPath(other_catalog, http.base_url()) !=
           cache.CoverPath(first_catalog, http.base_url()));
    for (const char* id : {"plain-id", "", ":song", "gateway:"}) {
        assert(http.BuildTrack(id, "song", "artist", false).provider.empty());
    }
    for (const char* alias : {"cover_url", "artwork_url", "album_art_url", "cover", "picUrl"}) {
        response_body = std::string(R"({"tracks":[{"id":"1","title":"song","url":"/stream/1",")") +
                        alias + R"(":"/cover/1?size=128"}]})";
        assert(http.Search("song", "", 1, tracks, error));
        assert(tracks[0].cover_url == "https://music.example/cover/1?size=128");
    }
    for (const char* invalid : {"file:///etc/passwd", "javascript:alert(1)",
                               "https://u:p@cdn.example/a", "http:///bad", "https://?a"}) {
        response_body = std::string(R"({"tracks":[{"title":"song","url":"/stream/1","cover_url":")") +
                        invalid + R"("}]})";
        assert(http.Search("song", "", 1, tracks, error));
        assert(tracks[0].cover_url.empty());
    }
    response_body = R"({"tracks":[{"title":"song","url":"/stream/1","cover_url":"//cdn.example/a"}]})";
    assert(http.Search("song", "", 1, tracks, error));
    assert(tracks[0].cover_url == "https://cdn.example/a");
    Http own, upstream;
    http.ApplyHeaders(saved.cover_url, own);
    http.ApplyHeaders(tracks[0].cover_url, upstream);
    assert(own.authorization == "Bearer key");
    assert(upstream.authorization.empty());
    SubsonicSource sub("https://navi.example", "me", "salt", "token", 128);
    response_body = R"({"subsonic-response":{"status":"ok","searchResult3":{"song":[{"id":"song-id","title":"song","coverArt":"album/a"}]}}})";
    assert(sub.Search("song", "", 1, tracks, error));
    assert(tracks[0].cover_url.find("/rest/getCoverArt.view?") != std::string::npos);
    assert(tracks[0].cover_url.find("id=album%2Fa&size=128") != std::string::npos);
    assert(tracks[0].cover_url.find("u=me&t=token&s=salt") != std::string::npos);
    auto favorite = sub.BuildTrack("song-id", "song", "artist", false);
    assert(favorite.cover_url.find("id=song-id&size=128") != std::string::npos);
    response_body = R"({"subsonic-response":{"status":"ok","searchResult3":{"song":[{"id":"song-id","title":"song"}]}}})";
    assert(sub.Search("song", "", 1, tracks, error));
    assert(tracks[0].cover_url.empty());
}
''')
            commands = [
                [os.environ.get("CC", "cc"), "-I" + str(CJSON), "-c", str(CJSON / "cJSON.c"),
                 "-o", str(folder / "cjson.o")],
                [os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 "-I" + str(folder), "-I" + str(MUSIC), "-I" + str(CJSON), str(driver),
                 str(MUSIC / "http_api_source.cc"), str(MUSIC / "subsonic_source.cc"),
                 str(MUSIC / "music_cache.cc"),
                 str(MUSIC / "music_util.cc"), str(MUSIC / "lrc_parser.cc"),
                 str(folder / "cjson.o"), "-o", str(folder / "test")],
                [str(folder / "test")],
            ]
            for command in commands:
                result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
