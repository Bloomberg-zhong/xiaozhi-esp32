#include "music_cache.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>

MusicTrack Song(const std::string& id) {
    MusicTrack track;
    track.id = id;
    track.title = id;
    track.provider = "gateway";
    track.cover_url = "https://cover.example/" + id + ".jpg";
    return track;
}

std::string Read(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string root = argv[1], base = "https://music.example";
    std::filesystem::create_directories(root);
    MusicCache cache(root, {8, 0});
    const auto song = Song("cached-local");
    auto writer = cache.Begin(song, base, 8);
    assert(writer && writer->Append(0, "ID3abcde", 8) && writer->Finish());
    writer.reset();
    auto entry = cache.Find(song, base);
    assert(entry);
    auto local = entry->track;
    entry.reset();
    const auto cover = cache.CoverPath(song, base);
    assert(cache.CoverPath(local, base) == cover);
    assert(cache.OriginalCoverUrl(local) == song.cover_url);
    assert(cache.OriginalCoverUrl(song).empty());
    assert(!cache.StoreCover(local, base, "pic", 3));
    assert(std::filesystem::exists(local.stream_url));  // Own audio stays pinned during GC.
    MusicCache writable(root, {11, 0});
    assert(writable.StoreCover(local, base, "pic", 3));
    assert(Read(cover) == "pic");
    const auto cover_marker = Read(cover + ".meta");
    std::filesystem::remove(cover + ".meta");
    std::ofstream(cover) << "user image";
    assert(!writable.StoreCover(local, base, "downloaded", 10));
    assert(Read(cover) == "user image");
    std::ofstream(cover) << "pic";
    std::ofstream(cover + ".meta") << cover_marker;
    local.cover_url = cover;
    assert(writable.OriginalCoverUrl(local) == song.cover_url);  // Metadata keeps the remote URL.
    std::fstream corrupt(local.stream_url, std::ios::in | std::ios::out | std::ios::binary);
    corrupt.put('X');
    corrupt.close();
    assert(!writable.Find(song, base));
    assert(writable.OriginalCoverUrl(local) == song.cover_url);
    assert(writable.CoverPath(local, base) == cover);  // Ownership lookup performs no audio CRC.

    MusicTrack manual;
    manual.id = manual.stream_url = root + "/manual.mp3";
    manual.title = "manual";
    manual.cover_url = "https://cover.example/manual.jpg";
    std::ofstream(manual.stream_url) << "ID3manual";
    assert(writable.CoverPath(manual, base).empty());
    assert(writable.OriginalCoverUrl(manual).empty());
    assert(!writable.StoreCover(manual, base, "pic", 3));
    assert(Read(manual.stream_url) == "ID3manual");

    // A marked file outside this cache cannot borrow its identity for writes.
    const auto outside = root + "/outside";
    std::filesystem::create_directories(outside);
    const auto filename = std::filesystem::path(local.stream_url).filename().string();
    std::filesystem::copy_file(local.stream_url, outside + "/" + filename);
    std::filesystem::copy_file(local.stream_url + ".meta", outside + "/" + filename + ".meta");
    auto forged = local;
    forged.stream_url = root + "/music-cache/../outside/" + filename;
    assert(writable.CoverPath(forged, base).empty());
    assert(writable.OriginalCoverUrl(forged).empty());
    assert(!writable.StoreCover(forged, base, "pic", 3));
    assert(!writable.PinArtwork(forged, base));
    forged.stream_url = outside + "/" + filename;
    assert(writable.CoverPath(forged, base).empty());
    assert(writable.OriginalCoverUrl(forged).empty());
    const auto link = root + "/music-cache/link " + filename;
    std::filesystem::create_symlink(outside + "/" + filename, link);
    std::filesystem::copy_file(local.stream_url + ".meta", link + ".meta");
    forged.stream_url = link;
    assert(writable.CoverPath(forged, base).empty());
    assert(writable.OriginalCoverUrl(forged).empty());
    assert(!writable.StoreCover(forged, base, "pic", 3));
    auto pin = writable.PinArtwork(local, base);
    assert(pin && !writable.Begin(Song("pressure"), base, 11));
    assert(Read(cover) == "pic");  // Local artwork pins use the original online guard.

    const auto no_http_root = root + "/no-http";
    std::filesystem::create_directories(no_http_root);
    MusicCache no_http(no_http_root, {8, 0});
    auto no_http_song = Song("without-http-cover");
    no_http_song.cover_url = root + "/manual-cover.jpg";
    writer = no_http.Begin(no_http_song, base, 8);
    assert(writer && writer->Append(0, "ID3abcde", 8) && writer->Finish());
    entry = no_http.Find(no_http_song, base);
    assert(entry && no_http.OriginalCoverUrl(entry->track).empty());

    const auto hash_root = root + "/hash-title";
    std::filesystem::create_directories(hash_root);
    MusicCache hash_cache(hash_root, {11, 0});
    auto hash_song = Song("hash-title");
    hash_song.title = "Symphony #40";
    hash_song.artist = "Mozart #W.A.";
    writer = hash_cache.Begin(hash_song, base, 8);
    assert(writer && writer->Append(0, "ID3abcde", 8) && writer->Finish());
    entry = hash_cache.Find(hash_song, base);
    assert(entry);
    const auto hash_local = entry->track;
    entry.reset();
    assert(hash_cache.OriginalCoverUrl(hash_local) == hash_song.cover_url);
    const auto hash_cover = hash_cache.CoverPath(hash_song, base);
    assert(hash_cache.CoverPath(hash_local, base) == hash_cover);
    assert(hash_cache.PinArtwork(hash_local, base));
    assert(hash_cache.StoreCover(hash_local, base, "pic", 3));
    assert(Read(hash_cover) == "pic");
}
