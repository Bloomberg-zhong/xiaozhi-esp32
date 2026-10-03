#include "music_cache.h"

#include <sys/statvfs.h>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

MusicTrack Song(const std::string& id) {
    MusicTrack track;
    track.id = id;
    track.title = id;
    track.provider = "gateway";
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
    const std::string base_root = argv[1];
    const auto root_for = [&](const std::string& name) {
        const auto root = base_root + "/" + name;
        std::filesystem::create_directories(root);
        return root;
    };
    const std::string old_base = "https://old.example", new_base = "https://new.example";
    const auto old_song = Song("old"), next_song = Song("next");
    {
        const auto root = root_for("quota");
        MusicCache cache(root, {96, 0});
        auto writer = cache.Begin(old_song, old_base, 64);
        assert(writer && writer->Append(0, "ID3a", 4) && writer->Suspend());
        writer.reset();
        const auto marker = PendingPath(root);
        const auto part = marker.substr(0, marker.size() - 8);
        std::ofstream(root + "/music-cache/user.mp3") << "user";
        std::ofstream(root + "/music-cache/.xzcache-user.part") << "manual partial";
        writer = cache.Begin(next_song, new_base, 64);
        assert(writer);  // Old-source reservation must not block the newly configured server.
        assert(!std::filesystem::exists(marker) && !std::filesystem::exists(part));
        assert(cache.Pending(old_base).empty());
        assert(std::filesystem::exists(root + "/music-cache/user.mp3"));
        assert(std::filesystem::exists(root + "/music-cache/.xzcache-user.part"));
    }
    {
        const auto root = root_for("headroom");
        struct statvfs space;
        assert(statvfs(root.c_str(), &space) == 0);
        const uint64_t available = uint64_t(space.f_bavail) * space.f_frsize;
        constexpr uint64_t megabyte = 1024 * 1024;
        assert(available > 24 * megabyte);
        MusicCache cache(root, {64 * megabyte, available - 24 * megabyte});
        auto writer = cache.Begin(old_song, old_base, 16 * megabyte);
        assert(writer && writer->Append(0, "ID3a", 4) && writer->Suspend());
        writer.reset();
        writer = cache.Begin(next_song, new_base, 16 * megabyte);
        assert(writer);  // Eviction must release unwritten free-space headroom as well as quota.
        assert(cache.Pending(old_base).empty());
    }
    {
        const auto root = root_for("pinned");
        MusicCache cache(root, {96, 0});
        auto writer = cache.Begin(old_song, old_base, 64);
        assert(writer && writer->Append(0, "ID3a", 4) && writer->Suspend());
        writer.reset();
        auto pin = cache.PinArtwork(old_song, old_base);
        assert(pin && !cache.Begin(next_song, new_base, 64));
        assert(cache.Pending(old_base).size() == 1);
        pin.reset();
        writer = cache.Begin(old_song, old_base, 64);
        assert(writer && writer->offset() == 4);        // Own resume is never a GC candidate.
        assert(!cache.Begin(next_song, new_base, 64));  // Live writer remains protected.
    }
    {
        const auto root = root_for("invalid-pairs");
        MusicCache cache(root, {96, 0});
        auto writer = cache.Begin(old_song, old_base, 64);
        assert(writer && writer->Append(0, "ID3a", 4) && writer->Suspend());
        writer.reset();
        const auto marker = PendingPath(root);
        const auto part = marker.substr(0, marker.size() - 8);
        std::filesystem::remove(part);
        writer = cache.Begin(next_song, new_base, 64);
        assert(writer && std::filesystem::exists(marker));  // Missing pair cannot reserve forever.
        writer.reset();
        std::ofstream(part) << "user changed the partial length";
        writer = cache.Begin(next_song, new_base, 64);
        assert(writer && std::filesystem::exists(part) && std::filesystem::exists(marker));
        writer.reset();
        std::ofstream(marker) << "not an owned pending marker";
        writer = cache.Begin(next_song, new_base, 64);
        assert(writer && std::filesystem::exists(part) && std::filesystem::exists(marker));
    }
    {
        const auto root = root_for("count");
        MusicCache cache(root, {4096, 0});
        std::string oldest_marker;
        for (int index = 0; index < 33; ++index) {
            auto writer = cache.Begin(Song("job-" + std::to_string(index)), old_base, 8);
            assert(writer && writer->Append(0, "ID3a", 4) && writer->Suspend());
            if (index == 0) {
                oldest_marker = PendingPath(root);
                std::filesystem::last_write_time(
                    oldest_marker.substr(0, oldest_marker.size() - 8),
                    std::filesystem::file_time_type::clock::now() - std::chrono::hours(1));
            }
        }
        assert(cache.Pending(old_base).size() == 32);
        assert(!std::filesystem::exists(oldest_marker));
        const auto jobs = cache.Pending(old_base);
        bool newest = false;
        for (const auto& track : jobs)
            newest = newest || track.id == "job-32";
        assert(newest);
        auto writer = cache.Begin(Song("job-32"), old_base, 8);
        assert(writer && writer->offset() == 4 && writer->Append(4, "b", 1));
        assert(writer->Suspend() && cache.Pending(old_base).size() == 32);
        writer.reset();
        std::vector<std::shared_ptr<MusicCache::Entry>> pins;
        for (const auto& track : cache.Pending(old_base))
            pins.push_back(cache.PinArtwork(track, old_base));
        writer = cache.Begin(Song("all-pinned"), old_base, 8);
        assert(writer && writer->Append(0, "ID3a", 4) && !writer->Suspend());
        assert(cache.Pending(old_base).size() == 32);
    }
    {
        const auto root = root_for("cover");
        MusicCache cache(root, {8, 0});
        auto writer = cache.Begin(old_song, old_base, 8);
        assert(writer && !cache.StoreCover(old_song, old_base, "pic", 3));
        assert(writer->Append(0, "ID3a", 4) && writer->Suspend());
        writer.reset();
        auto pin = cache.PinArtwork(old_song, old_base);
        assert(pin && !cache.StoreCover(old_song, old_base, "pic", 3));
    }
    {
        const auto root = root_for("artwork-pin");
        MusicCache cache(root, {16, 0});
        assert(cache.StoreCover(old_song, old_base, "coverpic", 8));
        auto pin = cache.PinArtwork(old_song, old_base);
        assert(pin);
        auto writer = cache.Begin(old_song, old_base, 8);
        assert(writer);  // An artwork reader must not block first-play audio caching.
        writer.reset();
        assert(!cache.Begin(next_song, new_base, 16));
        assert(std::filesystem::exists(cache.CoverPath(old_song, old_base)));
        pin.reset();
        writer = cache.Begin(next_song, new_base, 16);
        assert(writer && !std::filesystem::exists(cache.CoverPath(old_song, old_base)));
    }
    {
        const auto root = root_for("artwork-no-checksum");
        MusicCache cache(root, {16, 0});
        auto writer = cache.Begin(old_song, old_base, 8);
        assert(writer && writer->Append(0, "ID3abcde", 8) && writer->Finish());
        auto audio = cache.Find(old_song, old_base);
        assert(audio);
        const auto local_track = audio->track;
        audio.reset();
        std::fstream corrupt(local_track.stream_url, std::ios::in | std::ios::out);
        corrupt.put('X');
        corrupt.close();
        assert(!cache.Find(old_song, old_base));
        // Cover pinning reads ownership metadata only; audio validation belongs
        // to the audio reader and must not run again on the artwork task.
        assert(cache.PinArtwork(local_track, old_base));
    }
    {
        const auto root = root_for("cover-replace");
        MusicCache cache(root, {8, 0});
        assert(cache.StoreCover(old_song, old_base, "firstpic", 8));
        assert(cache.StoreCover(old_song, old_base, "second!!", 8));
        std::ifstream file(cache.CoverPath(old_song, old_base), std::ios::binary);
        std::string contents{std::istreambuf_iterator<char>(file), {}};
        assert(contents == "second!!");
    }
}
