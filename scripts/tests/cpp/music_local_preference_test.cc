#include "local_music.h"
#include "music_cache.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

std::vector<LyricLine> MusicSource::FetchLyrics(const MusicTrack&) { return {}; }

class OnlineSource : public MusicSource {
public:
    int searches = 0;
    std::string requested_source;
    const std::string base = "https://music.example";
    const char* type() const override { return "http"; }
    const std::string& base_url() const override { return base; }
    bool Search(const std::string&, const std::string& source, int, std::vector<MusicTrack>& tracks,
                std::string&) override {
        ++searches;
        requested_source = source;
        MusicTrack track;
        track.id = "gateway:kuwo:123";
        track.title = "Online song";
        track.provider = "gateway";
        track.stream_url = base + "/stream/123";
        tracks = {track};
        return true;
    }
    MusicTrack BuildTrack(const std::string&, const std::string&, const std::string&,
                          bool) const override {
        return {};
    }
    bool Ping(std::string&) override { return true; }
};

struct MusicPlayer {
    std::shared_ptr<MusicSource> online;
    std::string root;
    std::shared_ptr<MusicSource> GetSource() const { return online; }
    std::string GetLocalRoot() const { return root; }
};

// PRODUCTION_SELECTION

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string root = argv[1];
    std::filesystem::create_directories(root + "/children");
    std::filesystem::create_directories(root + "/white-noise");
    const std::string manual = root + "/children/Artist - Song.mp3";
    std::ofstream(manual) << "ID3manual";
    std::ofstream(root + "/children/Artist - Song.lrc") << "[00:01.00]manual lyric";
    std::ofstream(root + "/children/Other - Song.mp3") << "ID3other";
    std::ofstream(root + "/white-noise/Artist - Secret.mp3") << "ID3white";
    auto online = std::make_shared<OnlineSource>();
    MusicPlayer player{online, root};

    auto result = SearchMusic(player, "Artist Song", "", 20);
    assert(result.ok && result.tracks.size() == 1);
    assert(online->searches == 0);  // Existing SD file must prevent any online search/download.
    assert(result.tracks[0].stream_url == manual);
    assert(result.tracks[0].title == "Song" && result.tracks[0].artist == "Artist");
    assert(result.tracks[0].lyric_url == root + "/children/Artist - Song.lrc");
    result = SearchMusic(player, "Song", "", 1);
    assert(result.ok && result.tracks.size() == 1 && online->searches == 0);

    MusicTrack cached;
    cached.id = "gateway:kuwo:456";
    cached.provider = "gateway";
    cached.title = "Cached/Original";
    cached.artist = "Singer";
    cached.album = "Original album";
    cached.duration_ms = 123000;
    cached.lyric_text = "[00:01.00]cached lyric";
    const std::string audio = "ID3cached";
    MusicCache cache(root, {1024, 0});
    auto writer = cache.Begin(cached, online->base, audio.size());
    assert(writer && writer->Append(0, audio.data(), audio.size()) && writer->Finish());
    result = SearchMusic(player, "Cached Original Singer", "", 20);
    assert(result.ok && result.tracks.size() == 1 && online->searches == 0);
    assert(result.tracks[0].title == "Cached/Original");
    assert(result.tracks[0].album == "Original album" && result.tracks[0].duration_ms == 123000);
    assert(result.tracks[0].lyric_text == cached.lyric_text);
    const std::string cached_path = result.tracks[0].stream_url;
    result = SearchMusic(player, "Cached Original Singer", "", 20, true);
    assert(result.prepared_entry && result.prepared_entry->track.stream_url == cached_path);
    result = SearchOutcome{};  // release the pin before simulating external damage

    result = SearchMusic(player, "Artist Song", "gateway", 20);
    assert(result.ok && result.tracks[0].title == "Online song");
    assert(online->searches == 1 && online->requested_source == "gateway");
    for (const char* source : {"sdcard", "local"}) {
        result = SearchMusic(player, "Artist Song", source, 20);
        assert(result.ok && result.tracks[0].stream_url == manual && online->searches == 1);
        result = SearchMusic(player, "Missing song", source, 20);
        assert(!result.ok && online->searches == 1);  // Explicit SD selection stays on SD.
    }
    result = SearchMusic(player, "Missing song", "", 20);
    assert(result.ok && result.tracks[0].title == "Online song" && online->searches == 2);
    result = SearchMusic(player, "Artist Secret", "", 20);
    assert(result.ok && result.tracks[0].title == "Online song" && online->searches == 3);

    // A corrupted managed file must still fail the real scanner's integrity check.
    std::fstream corrupt(cached_path, std::ios::in | std::ios::out | std::ios::binary);
    corrupt.put('X');
    corrupt.close();
    result = SearchMusic(player, "Cached Original Singer", "", 20, true);
    assert(!result.prepared_entry);
    assert(result.ok && result.tracks[0].title == "Online song" && online->searches == 4);
    result = SearchMusic(player, "", "", 20);
    assert(result.ok && result.tracks[0].title == "Online song" && online->searches == 5);
    player.root.clear();
    result = SearchMusic(player, "Artist Song", "", 20);
    assert(result.ok && result.tracks[0].title == "Online song" && online->searches == 6);
    result = SearchMusic(player, "Artist Song", "sdcard", 20);
    assert(!result.ok && online->searches == 6);
    player.root = root + "/not-mounted";
    result = SearchMusic(player, "Artist Song", "", 20);
    assert(result.ok && result.tracks[0].title == "Online song" && online->searches == 7);
    player.root = root;
    player.online.reset();
    result = SearchMusic(player, "Artist Song", "", 20);
    assert(result.ok && result.tracks[0].stream_url == manual && online->searches == 7);
}
