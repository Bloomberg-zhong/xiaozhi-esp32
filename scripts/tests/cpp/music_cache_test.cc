#include "music_cache.h"
#include <unistd.h>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include "local_music.h"

std::string Read(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}
int main() {
    char temp[] = "/tmp/xz-cache-XXXXXX";
    std::string root = mkdtemp(temp);
    MusicTrack song;
    song.id = "song-42";
    song.provider = "provider";
    song.title = "Title/unsafe";
    song.artist = "Artist";
    song.album = "Album";
    song.duration_ms = 32000;
    song.stream_url = "https://source/song?token=secret";
    song.lyric_text = "[00:01.00]offline lyric";
    const std::string bytes = "ID3" + std::string(125, 'a');
    MusicCache cache(root, {256, 0});
    assert(!cache.Find(song, "https://source"));
    {
        auto writer = cache.Begin(song, "https://source", bytes.size());
        assert(writer);
        assert(writer->Append(0, bytes.data(), 2));
        assert(!cache.Find(song, "https://source"));
        assert(!writer->Finish());  // incomplete never appears as playable
    }
    assert(ScanLocalMusic(root).empty());
    {
        auto writer = cache.Begin(song, "https://source", bytes.size());
        assert(writer);
        assert(!cache.Begin(song, "https://source", bytes.size()));  // no duplicate writer
        assert(writer->Append(0, bytes.data(), bytes.size()));
        assert(writer->Finish());
    }
    auto hit = cache.Find(song, "https://source");
    assert(hit && Read(hit->track.stream_url) == bytes);
    assert(hit->track.title == song.title && hit->track.artist == song.artist);
    assert(hit->track.album == song.album && hit->track.duration_ms == 32000);
    assert(hit->track.lyric_text == song.lyric_text);
    song.stream_url = "https://source/song?token=changed";
    assert(cache.Find(song, "https://source"));  // stable id ignores signed URL
    hit.reset();
    assert(
        !cache.Begin(song, "https://source", bytes.size()));  // completed song cannot be rewritten
    hit = cache.Find(song, "https://source");
    assert(hit);
    assert(!cache.Find(song, "https://other-source"));
    song.provider = "other";
    assert(!cache.Find(song, "https://source"));
    song.provider = "provider";
    auto offline = ScanLocalMusic(root);
    assert(offline.size() == 1 && offline[0].title == "Title/unsafe");
    // Cached bytes mutated at same length must not be accepted.
    {
        std::fstream f(hit->track.stream_url, std::ios::in | std::ios::out);
        f.put('X');
    }
    assert(!cache.Find(song, "https://source"));
    assert(ScanLocalMusic(root).empty());
    LocalMusicScanOptions fast_scan;
    fast_scan.verify_cache_audio = false;
    auto fast_list = ScanLocalMusic(root, fast_scan);
    assert(fast_list.size() == 1 && fast_list[0].title == song.title);
    // Listing may defer bitrot detection, but playback must still reject it.
    assert(!cache.PinLocal(fast_list[0].stream_url));
    std::filesystem::resize_file(hit->track.stream_url, bytes.size() - 1);
    assert(ScanLocalMusic(root, fast_scan).empty());  // truncated audio is never listed
    std::string first_path = hit->track.stream_url;
    hit.reset();
    {
        auto writer = cache.Begin(song, "https://source", bytes.size());
        assert(writer && writer->Append(0, bytes.data(), bytes.size()) && writer->Finish());
    }
    hit = cache.Find(song, "https://source");
    MusicTrack second = song;
    second.id = "second";
    assert(!cache.Begin(second, "https://source", 257));  // exceeds budget
    assert(!cache.Begin(second, "https://source", 0));    // unknown length
    assert(!MusicCache("", {256, 0}).Begin(song, "source", 128));
    second.live = true;
    assert(!cache.Begin(second, "https://source", 128));
    second.live = false;
    {
        auto writer = cache.Begin(second, "https://source", 128);
        assert(writer);
    }
    assert(!cache.Find(second, "https://source"));  // cancellation removes part
    {
        auto writer = cache.Begin(second, "https://source", 128);
        assert(writer);
        assert(!writer->Append(1, bytes.data(), 128));
        assert(!writer->Finish());
    }
    {
        auto writer = cache.Begin(second, "https://source", 128);
        assert(writer);
        std::string bad(128, 'x');
        assert(writer->Append(0, bad.data(), bad.size()));
        assert(!writer->Finish());
    }
    {
        auto writer = cache.Begin(second, "https://source", 128);
        assert(writer);
        assert(!writer->Append(0, bytes.data(), 129));
    }
    {
        auto writer = cache.Begin(second, "https://source", 128);
        assert(writer && writer->Append(0, bytes.data(), 64));
        assert(writer->Suspend());
        auto pending = cache.Pending("https://source");
        assert(pending.size() == 1 && pending[0].id == "second");
        assert(pending[0].stream_url.empty());  // signed URLs never persisted
        assert(!cache.Find(second, "https://source"));
        writer = cache.Begin(second, "https://source", 128);
        assert(writer && writer->offset() == 64);
        assert(writer->Append(64, bytes.data() + 64, 64) && writer->Finish());
        assert(cache.Pending("https://source").empty());
    }
    {
        MusicCache roomy(root, {512, 0});
        MusicTrack corrupt = song;
        corrupt.id = "corrupt-part";
        auto writer = roomy.Begin(corrupt, "https://source", 128);
        assert(writer && writer->Append(0, bytes.data(), 64) && writer->Suspend());
        for (const auto& file : std::filesystem::directory_iterator(root + "/music-cache")) {
            if (file.path().extension() == ".part") {
                std::fstream f(file.path(), std::ios::in | std::ios::out);
                f.seekp(50);
                f.put('z');
            }
        }
        writer = roomy.Begin(corrupt, "https://source", 128);
        assert(writer && writer->offset() == 0);  // reject bitrotted partial and restart
    }
    std::ofstream(root + "/music-cache/user.mp3") << "user song";
    std::filesystem::create_directories(root + "/music-cache/user-child");
    std::ofstream(root + "/music-cache/user-child/keep.mp3") << "keep";
    {
        auto pending_hit = cache.Find(second, "https://source");
        assert(pending_hit);
        std::filesystem::remove(pending_hit->track.stream_url);
        std::filesystem::remove(pending_hit->track.stream_url + ".meta");
    }
    MusicCache small(root, {128, 0});
    assert(!small.Begin(second, "https://source", 128));  // pinned playing track retained
    hit.reset();
    {
        auto writer = small.Begin(second, "https://source", 128);
        assert(writer);
        assert(writer->Append(0, bytes.data(), bytes.size()) && writer->Finish());
    }
    assert(!std::filesystem::exists(first_path));
    assert(Read(root + "/music-cache/user.mp3") == "user song");
    assert(Read(root + "/music-cache/user-child/keep.mp3") == "keep");
    {
        const std::string cover = "image-bytes";
        assert(cache.StoreCover(second, "https://source", cover.data(), cover.size()));
        assert(Read(cache.CoverPath(second, "https://source")) == cover);
        auto entry = cache.Find(second, "https://source");
        assert(entry && entry->track.cover_url == cache.CoverPath(second, "https://source"));
        assert(!cache.StoreCover(second, "https://source", cover.data(), 65537));
    }
    assert(!MusicCache(root, {256, UINT64_MAX}).Begin(song, "https://source", 128));
    std::filesystem::remove_all(root);
    char pending_temp[] = "/tmp/xz-cache-pending-XXXXXX";
    root = mkdtemp(pending_temp);
    MusicCache pending_cache(root, {4096, 0});
    for (int i = 0; i < 33; ++i) {
        MusicTrack pending_song = song;
        pending_song.id = "pending-" + std::to_string(i);
        auto writer = pending_cache.Begin(pending_song, "https://source", 8);
        assert(writer && writer->Append(0, "ID3a", 4));
        assert(writer->Suspend());  // New jobs evict the oldest inactive owned partial at capacity.
    }
    assert(pending_cache.Pending("https://source").size() == 32);
    std::filesystem::remove_all(root);
    std::cout << "music cache tests passed\n";
}
