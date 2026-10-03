#include "music_cache.h"

#include <unistd.h>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "lrc_parser.h"

std::string Read(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}

int main() {
    char temp[] = "/tmp/xz-lyric-boundaries-XXXXXX";
    const std::string root = mkdtemp(temp);
    MusicTrack song;
    song.id = "song";
    song.title = "Song";
    song.artist = "Artist";
    song.stream_url = "https://source/signed?secret=hidden";
    const std::vector<LyricLine> lines{{1234, "lyrics"}};
    const std::string want = "[00:01.234] lyrics\n";  // 19 bytes, plus 8 audio bytes.
    MusicCache constrained(root, {19, 0});
    {
        auto writer = constrained.Begin(song, "source", 8);
        assert(writer);
        // A live writer's reserved audio bytes count against lyric headroom.
        assert(!constrained.StoreLyrics(song, "source", lines));
        assert(writer->Append(0, "ID3abcde", 8) && writer->Finish());
    }
    MusicCache cache(root, {27, 0});
    auto hit = cache.Find(song, "source");
    assert(hit);
    assert(cache.StoreLyrics(song, "source", lines));
    const std::string lyric =
        hit->track.stream_url.substr(0, hit->track.stream_url.rfind('.')) + ".lrc";
    assert(Read(lyric) == want);
    std::string text;
    assert(cache.ReadLyrics(song, "source", text) && text == want);
    assert(!cache.ReadLyrics(song, "other-source", text));
    MusicTrack renamed = song;
    renamed.title = "Renamed";
    assert(cache.ReadLyrics(renamed, "source", text) && text == want);
    // Updating a companion of equal size can reuse its existing cache budget.
    assert(cache.StoreLyrics(song, "source", {{1234, "edited"}}));
    assert(Read(lyric) == "[00:01.234] edited\n");
    assert(!cache.StoreLyrics(song, "source", {{1234, std::string(65536, 'a')}}));
    assert(!cache.StoreLyrics(song, "source", std::vector<LyricLine>(513, {0, "a"})));
    assert(!cache.StoreLyrics(song, "source", {{1234, "bad\n[00:00]injected"}}));
    assert(Read(lyric) == "[00:01.234] edited\n");
    {
        std::fstream file(lyric, std::ios::in | std::ios::out);
        file.put('X');
    }
    assert(!cache.ReadLyrics(song, "source", text));
    MusicTrack offline;
    assert(MusicCache::ReadTrack(hit->track.stream_url, offline) && offline.lyric_url.empty());
    assert(cache.StoreLyrics(song, "source", lines));
    assert(MusicCache::ReadTrack(hit->track.stream_url, offline) && offline.lyric_url == lyric);
    // A failed/stale temporary write must not be truncated or published.
    std::ofstream(lyric + ".part") << "user temporary";
    assert(!cache.StoreLyrics(song, "source", {{1234, "edited"}}));
    assert(Read(lyric + ".part") == "user temporary" && Read(lyric) == want);
    std::filesystem::remove(lyric + ".part");
    // Losing the marker removes permission to overwrite or garbage-collect.
    std::filesystem::remove(lyric + ".meta");
    std::ofstream(lyric) << "user lyrics";
    assert(!cache.StoreLyrics(song, "source", lines));
    assert(Read(lyric) == "user lyrics");
    hit.reset();
    assert(!MusicCache("", {27, 0}).StoreLyrics(song, "source", lines));
    song.live = true;
    assert(!cache.StoreLyrics(song, "source", lines));
    song.live = false;
    MusicTrack second = song;
    second.id = "second";
    MusicCache roomy(root, {64, 0});
    assert(roomy.StoreLyrics(second, "source", lines));
    // The managed lyric is a budgeted GC candidate; user's same-name LRC survives.
    MusicCache gc(root, {8, 0});
    auto writer = gc.Begin(second, "source", 8);
    assert(writer && writer->Append(0, "ID3abcde", 8) && writer->Finish());
    assert(!roomy.ReadLyrics(second, "source", text));
    assert(Read(lyric) == "user lyrics");
    std::filesystem::remove_all(root);
}
