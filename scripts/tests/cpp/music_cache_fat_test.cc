#include "music_cache.h"

#include <sys/stat.h>
#include <cassert>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <string>

namespace fat_test {
std::string fail_destination;
int failures = 0;
int Rename(const char* from, const char* to) {
    struct stat info;
    if (stat(to, &info) == 0) {
        errno = EEXIST;
        return -1;
    }
    if (fail_destination == to && failures > 0) {
        --failures;
        errno = EIO;
        return -1;
    }
    return std::rename(from, to);
}
}  // namespace fat_test

std::string Read(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}
MusicTrack Song(const std::string& id) {
    MusicTrack track;
    track.id = id;
    track.provider = "gateway";
    track.title = id;
    return track;
}
std::string PendingPath(const std::string& root) {
    for (const auto& file : std::filesystem::directory_iterator(root + "/music-cache")) {
        if (file.path().extension() == ".pending")
            return file.path();
    }
    return "";
}

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string root = argv[1], base = "https://source.example";
    std::filesystem::create_directories(root);
    MusicCache cache(root, {4096, 0});
    const auto song = Song("repeated-suspend");
    auto writer = cache.Begin(song, base, 8);
    assert(writer && writer->Append(0, "ID3a", 4) && writer->Suspend());
    writer.reset();
    const auto marker = PendingPath(root);
    writer = cache.Begin(song, base, 8);
    assert(writer && writer->offset() == 4 && writer->Append(4, "bc", 2));
    assert(writer->Suspend());  // FAT rejects rename over the first pending marker.
    writer.reset();
    writer = cache.Begin(song, base, 8);
    assert(writer && writer->offset() == 6 && writer->Append(6, "de", 2));
    fat_test::fail_destination = marker;
    fat_test::failures = 1;
    assert(!writer->Suspend());
    writer.reset();
    fat_test::fail_destination.clear();
    assert(cache.Pending(base).size() == 1);  // Latest temporary and older marker count once.
    writer = cache.Begin(song, base, 8);
    assert(writer && writer->offset() == 8);  // Failed publication must retain the newest prefix.
    assert(writer->Finish());
    writer.reset();
    auto entry = cache.Find(song, base);
    assert(entry && Read(entry->track.stream_url) == "ID3abcde");
    const auto audio = entry->track.stream_url;
    entry.reset();
    std::fstream corrupt(audio, std::ios::in | std::ios::out | std::ios::binary);
    corrupt.put('X');
    corrupt.close();
    writer = cache.Begin(song, base, 8);
    assert(writer && writer->Append(0, "ID3fixed", 8) && writer->Finish());
    writer.reset();
    assert(cache.Find(song, base) && Read(audio) == "ID3fixed");

    assert(cache.StoreCover(song, base, "old-image", 9));
    assert(cache.StoreCover(song, base, "new-image", 9));
    const auto cover = cache.CoverPath(song, base);
    fat_test::fail_destination = cover;
    fat_test::failures = 1;
    assert(!cache.StoreCover(song, base, "bad-image", 9));
    assert(Read(cover) == "new-image");
    fat_test::fail_destination.clear();
    assert(cache.StoreCover(song, base, "final-img", 9));
    std::ofstream(cover + ".bak") << "user backup";
    assert(!cache.StoreCover(song, base, "overwrite", 9));
    assert(Read(cover) == "final-img" && Read(cover + ".bak") == "user backup");
    std::filesystem::remove(cover + ".bak");
    assert(cache.StoreLyrics(song, base, {{1000, "old lyric"}}));
    assert(cache.StoreLyrics(song, base, {{1000, "new lyric"}}));
    std::string lyrics;
    assert(cache.ReadLyrics(song, base, lyrics) && lyrics.find("new lyric") != std::string::npos);
    const auto lyric_path = cover.substr(0, cover.size() - 10) + ".lrc";
    fat_test::fail_destination = lyric_path;
    fat_test::failures = 1;
    assert(!cache.StoreLyrics(song, base, {{2000, "longer broken replacement"}}));
    assert(cache.ReadLyrics(song, base, lyrics) && lyrics.find("new lyric") != std::string::npos);
    fat_test::fail_destination.clear();
    assert(cache.StoreLyrics(song, base, {{3000, "last lyric"}}));

    const auto user_song = Song("user-file");
    const auto user_cover = cache.CoverPath(user_song, base);
    std::ofstream(user_cover) << "user cover";
    assert(!cache.StoreCover(user_song, base, "downloaded", 10));
    assert(Read(user_cover) == "user cover");
    const auto user_lyric = user_cover.substr(0, user_cover.size() - 10) + ".lrc";
    std::ofstream(user_lyric) << "user lyrics";
    assert(!cache.StoreLyrics(user_song, base, {{1000, "downloaded lyric"}}));
    assert(Read(user_lyric) == "user lyrics");
    const auto repair_song = Song("unmarked-audio");
    const auto repair_cover = cache.CoverPath(repair_song, base);
    const auto user_audio = repair_cover.substr(0, repair_cover.size() - 10) + ".mp3";
    std::ofstream(user_audio) << "user mp3";
    writer = cache.Begin(repair_song, base, 8);
    assert(writer && writer->Append(0, "ID3saved", 8) && !writer->Finish());
    assert(Read(user_audio) == "user mp3");
    const auto user_part_song = Song("user-partial");
    writer = cache.Begin(user_part_song, base, 8);
    assert(writer);
    const auto user_part = writer->PartialPath();
    writer.reset();
    std::ofstream(user_part) << "user part";
    assert(!cache.Begin(user_part_song, base, 8) && Read(user_part) == "user part");
    // A truncated (missing EOF payload) marker never authorizes replacement.
    std::ofstream(user_cover + ".meta") << "XIAOZHI-MUSIC-COVER-1";
    assert(!cache.StoreCover(user_song, base, "downloaded", 10));
    assert(Read(user_cover) == "user cover");
    for (const char* failing_file : {"audio", "metadata"}) {
        const auto final_song = Song(std::string("final-failure-") + failing_file);
        const auto final_cover = cache.CoverPath(final_song, base);
        const auto final_audio = final_cover.substr(0, final_cover.size() - 10) + ".mp3";
        writer = cache.Begin(final_song, base, 8);
        assert(writer && writer->Append(0, "ID3r", 4) && writer->Suspend());
        writer.reset();
        writer = cache.Begin(final_song, base, 8);
        assert(writer && writer->offset() == 4 && writer->Append(4, "etry", 4));
        const auto prefix = writer->PartialPath();
        fat_test::fail_destination =
            std::string(failing_file) == "audio" ? final_audio : final_audio + ".meta";
        fat_test::failures = 1;
        assert(!writer->Finish());
        writer.reset();
        assert(Read(prefix) == "ID3retry");
        fat_test::fail_destination.clear();
        writer = cache.Begin(final_song, base, 8);
        assert(writer && writer->offset() == 8);  // Retry publication with no extra audio download.
        assert(writer->Finish());
        writer.reset();
        assert(cache.Find(final_song, base) && Read(final_audio) == "ID3retry");
    }
    const auto backup_song = Song("final-user-backup");
    const auto backup_cover = cache.CoverPath(backup_song, base);
    const auto backup_audio = backup_cover.substr(0, backup_cover.size() - 10) + ".mp3";
    std::ofstream(backup_audio + ".bak") << "user audio backup";
    writer = cache.Begin(backup_song, base, 8);
    assert(writer && writer->Append(0, "ID3retry", 8) && !writer->Finish());
    writer.reset();
    assert(Read(backup_audio + ".bak") == "user audio backup");
    writer = cache.Begin(backup_song, base, 8);
    assert(writer && writer->offset() == 8);
    std::filesystem::remove(backup_audio + ".bak");
    assert(writer->Finish());
}
