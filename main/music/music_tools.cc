#include "music_tools.h"

#include <esp_random.h>

#include <array>
#include <memory>

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "display.h"
#include "favorites.h"
#include "local_music.h"
#include "mcp_server.h"
#include "music_player.h"
#include "music_source.h"
#include "music_util.h"
#include "settings.h"
#include "subsonic_source.h"

namespace {

constexpr int kSearchLimit = 20;
constexpr size_t kMaxLocalQueue = 200;
constexpr size_t kMaxListed = 30;
constexpr const char* kSettingsNamespace = "music";
constexpr const char* kFavoritesKey = "favs";

cJSON* MakeResult(bool success, const std::string& message) {
    cJSON* result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "success", success);
    if (!message.empty()) {
        cJSON_AddStringToObject(result, "message", message.c_str());
    }
    return result;
}

cJSON* MakeTrackJson(const MusicTrack& track) {
    cJSON* item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "title", track.title.c_str());
    cJSON_AddStringToObject(item, "artist", track.artist.c_str());
    if (!track.album.empty()) {
        cJSON_AddStringToObject(item, "album", track.album.c_str());
    }
    if (!track.provider.empty()) {
        cJSON_AddStringToObject(item, "source", track.provider.c_str());
    }
    if (track.live) {
        cJSON_AddBoolToObject(item, "live", true);
    }
    return item;
}

const char* ModeText(MusicPlayMode mode) {
    switch (mode) {
        case MusicPlayMode::kRepeatAll:
            return Lang::Strings::MUSIC_MODE_REPEAT_ALL;
        case MusicPlayMode::kRepeatOne:
            return Lang::Strings::MUSIC_MODE_REPEAT_ONE;
        case MusicPlayMode::kShuffle:
            return Lang::Strings::MUSIC_MODE_SHUFFLE;
        default:
            return Lang::Strings::MUSIC_MODE_SEQUENCE;
    }
}

// Applies and saves a play mode, and shows its name on the display.
void ApplyPlayMode(MusicPlayMode mode) {
    Application::GetInstance().GetMusicPlayer().SetPlayMode(mode);
    {
        Settings settings(kSettingsNamespace, true);
        settings.SetString("mode", MusicPlayModeName(mode));
    }
    Application::GetInstance().Schedule(
        [mode]() { Board::GetInstance().GetDisplay()->ShowNotification(ModeText(mode), 2500); });
}

FavoriteList LoadFavorites() {
    Settings settings(kSettingsNamespace, false);
    return FavoriteList::Parse(settings.GetString(kFavoritesKey));
}

void SaveFavorites(const FavoriteList& list) {
    Settings settings(kSettingsNamespace, true);
    settings.SetString(kFavoritesKey, list.Serialize());
}

FavoriteEntry ToFavorite(const MusicTrack& track) {
    FavoriteEntry entry;
    entry.title = track.title;
    entry.artist = track.artist;
    if (IsLocalMusicPath(track.stream_url)) {
        entry.kind = 'l';
        entry.id = track.stream_url;
    } else {
        entry.kind = track.live ? 'r' : 's';
        entry.id = track.id;
    }
    return entry;
}

MusicTrack LocalTrackFromPath(const FavoriteEntry& entry) {
    MusicTrack track;
    track.id = entry.id;
    track.title = entry.title;
    track.artist = entry.artist;
    track.stream_url = entry.id;
    size_t dot = entry.id.rfind('.');
    if (dot != std::string::npos) {
        track.lyric_url = entry.id.substr(0, dot) + ".lrc";  // Missing files are ignored
    }
    return track;
}

// Finds songs on the SD card, or online when `source` names a catalog (or is
// empty and a music source is configured).
struct SearchOutcome {
    std::vector<MusicTrack> tracks;
    std::string error;
    bool ok = false;
};

SearchOutcome SearchMusic(MusicPlayer& player, const std::string& query, const std::string& source,
                          int limit) {
    SearchOutcome outcome;
    auto online = player.GetSource();
    const bool want_local = source == "sdcard" || source == "local" || (source.empty() && !online);
    if (want_local) {
        const std::string root = player.GetLocalRoot();
        if (root.empty()) {
            outcome.error =
                online ? "No SD card with music is available"
                       : "No music source is configured and there is no SD card with music. The "
                         "user can set up a music server with self.music.configure_source in the "
                         "device console.";
            return outcome;
        }
        LocalMusicScanOptions options;
        options.excluded_folders.push_back(kWhiteNoiseFolder);
        outcome.tracks = FilterLocalMusic(ScanLocalMusic(root, options), query);
        if (outcome.tracks.size() > static_cast<size_t>(limit)) {
            outcome.tracks.resize(limit);
        }
        outcome.ok = !outcome.tracks.empty();
        if (!outcome.ok) {
            outcome.error = "No matching music files on the SD card";
        }
        return outcome;
    }
    if (!online) {
        outcome.error = "No online music source is configured";
        return outcome;
    }
    outcome.ok = online->Search(query, source, limit, outcome.tracks, outcome.error);
    return outcome;
}

bool ParseModeArgument(const std::string& name, MusicPlayMode& mode, std::string& error) {
    if (!ParseMusicPlayMode(name, mode)) {
        error = "Unknown mode: " + name + ". Use sequence, repeat_all, repeat_one or shuffle.";
        return false;
    }
    return true;
}

}  // namespace

void LoadMusicSettings(MusicPlayer& player) {
    player.SetSource(CreateMusicSource(MusicSourceConfig::Load()));
    if (const char* local_path = Board::GetInstance().GetLocalMusicPath()) {
        player.SetLocalRoot(local_path);
    }
    Settings settings(kSettingsNamespace, false);
    MusicPlayMode mode;
    if (ParseMusicPlayMode(settings.GetString("mode", "sequence"), mode)) {
        player.SetPlayMode(mode);
    }
}

void CycleMusicPlayMode() {
    auto& player = Application::GetInstance().GetMusicPlayer();
    constexpr MusicPlayMode kOrder[] = {MusicPlayMode::kSequence, MusicPlayMode::kRepeatAll,
                                        MusicPlayMode::kRepeatOne, MusicPlayMode::kShuffle};
    MusicPlayMode current = player.GetPlayMode();
    size_t index = 0;
    for (size_t i = 0; i < 4; ++i) {
        if (kOrder[i] == current) {
            index = i;
        }
    }
    ApplyPlayMode(kOrder[(index + 1) % 4]);
}

void AddMusicTools(McpServer& server) {
    auto play = std::make_unique<McpTool>(
        "self.music.play",
        "Play music. Use it whenever the user asks for a song, singer, album, radio or just "
        "\"some music\". Playback starts right after your reply, so keep the reply short, for "
        "example \"好的，为你播放稻香\".\n"
        "Args:\n"
        "  `query`: Title, artist or station keywords, e.g. \"稻香 周杰伦\". Empty for random "
        "songs.\n"
        "  `source`: Where to look: empty for the default, `sdcard` for files on the SD card, "
        "`radio` for live radio stations, `jamendo` or `archive` for free online music, or "
        "another catalog of the music server.\n"
        "  `mode`: Optional play mode: `sequence`, `repeat_all` (repeat the list), `repeat_one` "
        "(repeat one song) or `shuffle`. Empty keeps the current mode.",
        PropertyList({Property("query", kPropertyTypeString, std::string("")),
                      Property("source", kPropertyTypeString, std::string("")),
                      Property("mode", kPropertyTypeString, std::string(""))}),
        [](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            auto& player = app.GetMusicPlayer();
            MusicPlayMode mode = player.GetPlayMode();
            const std::string mode_name = properties["mode"].value<std::string>();
            std::string error;
            if (!mode_name.empty() && !ParseModeArgument(mode_name, mode, error)) {
                return MakeResult(false, error);
            }
            const std::string source = properties["source"].value<std::string>();
            auto found =
                SearchMusic(player, properties["query"].value<std::string>(), source, kSearchLimit);
            if (!found.ok) {
                return MakeResult(false, found.error);
            }
            cJSON* result = MakeResult(true, "");
            cJSON_AddItemToObject(result, "now_playing", MakeTrackJson(found.tracks.front()));
            cJSON_AddNumberToObject(result, "queue_length", found.tracks.size());
            const bool local = IsLocalMusicPath(found.tracks.front().stream_url);
            player.SetQueue(std::move(found.tracks), 0, false, local ? "local" : "online");
            if (!mode_name.empty()) {
                ApplyPlayMode(mode);
            }
            app.PlayMusic(true);
            return result;
        });
    play->set_async(true);
    server.AddTool(std::move(play));

    server.AddTool(
        "self.music.control",
        "Control the current music.\n"
        "Args:\n"
        "  `action`: `pause` (keeps the position), `resume`, `next`, `previous` or `stop`.",
        PropertyList({Property("action", kPropertyTypeString)}),
        [](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            auto& player = app.GetMusicPlayer();
            const std::string action = properties["action"].value<std::string>();
            if (action == "pause") {
                app.PauseMusic();
            } else if (action == "stop") {
                app.StopMusic();
            } else if (action == "resume" || action == "next" || action == "previous") {
                if (!player.HasTrack()) {
                    return MakeResult(false, "Nothing is queued. Use self.music.play first.");
                }
                if (action == "resume") {
                    app.PlayMusic(false);
                } else {
                    app.SkipMusic(action == "next");
                }
            } else {
                return MakeResult(false, "Unknown action: " + action);
            }
            return MakeResult(true, "");
        });

    server.AddTool(
        "self.music.set_play_mode",
        "Set how the music continues: `sequence` (stop after the last song), `repeat_all` "
        "(列表循环), `repeat_one` (单曲循环) or `shuffle` (随机播放).",
        PropertyList({Property("mode", kPropertyTypeString)}),
        [](const PropertyList& properties) -> ReturnValue {
            MusicPlayMode mode;
            std::string error;
            if (!ParseModeArgument(properties["mode"].value<std::string>(), mode, error)) {
                return MakeResult(false, error);
            }
            ApplyPlayMode(mode);
            return MakeResult(true, "");
        });

    auto queue = std::make_unique<McpTool>(
        "self.music.queue",
        "Show or edit the playlist (queue) of the current music. Positions start at 1.\n"
        "Args:\n"
        "  `action`: `list`, `play` (jump to a position), `add` (append songs), `add_next` "
        "(play after the current song), `remove` or `clear`.\n"
        "  `index`: Position for `play` and `remove`.\n"
        "  `query`, `source`, `count`: What to search for `add` and `add_next`, like "
        "self.music.play, and how many songs to add (1-10).",
        PropertyList({Property("action", kPropertyTypeString),
                      Property("index", kPropertyTypeInteger, 0, 0, 200),
                      Property("query", kPropertyTypeString, std::string("")),
                      Property("source", kPropertyTypeString, std::string("")),
                      Property("count", kPropertyTypeInteger, 1, 1, 10)}),
        [](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            auto& player = app.GetMusicPlayer();
            const std::string action = properties["action"].value<std::string>();
            const int index = properties["index"].value<int>();

            std::vector<MusicTrack> tracks;
            size_t current = 0;
            player.GetQueue(tracks, current);

            if (action == "list") {
                cJSON* result = MakeResult(true, "");
                cJSON_AddNumberToObject(result, "total", tracks.size());
                cJSON_AddNumberToObject(result, "current", tracks.empty() ? 0 : current + 1);
                cJSON_AddStringToObject(result, "play_mode",
                                        MusicPlayModeName(player.GetPlayMode()));
                cJSON* list = cJSON_CreateArray();
                // Show the songs around the current one when the list is long.
                size_t first = current >= 5 && tracks.size() > kMaxListed ? current - 5 : 0;
                for (size_t i = first; i < tracks.size() && i < first + kMaxListed; ++i) {
                    cJSON* item = MakeTrackJson(tracks[i]);
                    cJSON_AddNumberToObject(item, "index", i + 1);
                    cJSON_AddItemToArray(list, item);
                }
                cJSON_AddItemToObject(result, "songs", list);
                return result;
            }
            if (action == "play") {
                if (index < 1 || !player.SelectIndex(index - 1)) {
                    return MakeResult(false, "There is no song at that position");
                }
                app.PlayMusic(true);
                return MakeResult(true, "");
            }
            if (action == "remove") {
                if (index < 1 || !player.RemoveFromQueue(index - 1)) {
                    return MakeResult(false, "There is no song at that position");
                }
                if (static_cast<size_t>(index - 1) == current && player.WantsPlayback()) {
                    if (player.HasTrack()) {
                        app.PlayMusic(true);  // The next song took the place
                    } else {
                        app.StopMusic();
                    }
                }
                return MakeResult(true, "");
            }
            if (action == "clear") {
                app.StopMusic();
                player.ClearQueue();
                return MakeResult(true, "");
            }
            if (action == "add" || action == "add_next") {
                const int count = properties["count"].value<int>();
                auto found = SearchMusic(player, properties["query"].value<std::string>(),
                                         properties["source"].value<std::string>(), count);
                if (!found.ok) {
                    return MakeResult(false, found.error);
                }
                const bool was_empty = tracks.empty();
                const size_t added =
                    player.AddToQueue(std::move(found.tracks), action == "add_next");
                if (added == 0) {
                    return MakeResult(false, "The playlist is full");
                }
                if (was_empty) {
                    player.SelectIndex(0);
                    app.PlayMusic(true);
                }
                cJSON* result = MakeResult(true, "");
                cJSON_AddNumberToObject(result, "added", added);
                return result;
            }
            return MakeResult(false, "Unknown action: " + action);
        });
    queue->set_async(true);
    server.AddTool(std::move(queue));

    auto favorites = std::make_unique<McpTool>(
        "self.music.favorites",
        "The user's favorite songs (收藏). Positions start at 1.\n"
        "Args:\n"
        "  `action`: `list`, `add` (the current song), `remove` or `play` (the whole list, "
        "starting at `index`, or at the first song when `index` is 0).\n"
        "  `index`: Position for `remove` and `play`.",
        PropertyList({Property("action", kPropertyTypeString),
                      Property("index", kPropertyTypeInteger, 0, 0, 100)}),
        [](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            auto& player = app.GetMusicPlayer();
            const std::string action = properties["action"].value<std::string>();
            const int index = properties["index"].value<int>();
            FavoriteList list = LoadFavorites();

            if (action == "list") {
                cJSON* result = MakeResult(true, "");
                cJSON* songs = cJSON_CreateArray();
                for (size_t i = 0; i < list.size(); ++i) {
                    const auto& entry = list.items()[i];
                    cJSON* item = cJSON_CreateObject();
                    cJSON_AddNumberToObject(item, "index", i + 1);
                    cJSON_AddStringToObject(item, "title", entry.title.c_str());
                    cJSON_AddStringToObject(item, "artist", entry.artist.c_str());
                    cJSON_AddItemToArray(songs, item);
                }
                cJSON_AddItemToObject(result, "songs", songs);
                return result;
            }
            if (action == "add") {
                MusicTrack track;
                if (!player.GetCurrentTrack(track)) {
                    return MakeResult(false, "Nothing is playing");
                }
                if (!list.Add(ToFavorite(track))) {
                    return MakeResult(false, "This song cannot be saved as a favorite");
                }
                SaveFavorites(list);
                return MakeResult(true, "");
            }
            if (action == "remove") {
                if (index < 1 || !list.Remove(index - 1)) {
                    return MakeResult(false, "There is no favorite at that position");
                }
                SaveFavorites(list);
                return MakeResult(true, "");
            }
            if (action == "play") {
                if (list.size() == 0) {
                    return MakeResult(false, "There are no favorite songs yet");
                }
                auto online = player.GetSource();
                std::vector<MusicTrack> tracks;
                size_t start = 0;
                for (size_t i = 0; i < list.size(); ++i) {
                    const auto& entry = list.items()[i];
                    if (entry.kind == 'l') {
                        tracks.push_back(LocalTrackFromPath(entry));
                    } else if (online) {
                        tracks.push_back(online->BuildTrack(entry.id, entry.title, entry.artist,
                                                            entry.kind == 'r'));
                    } else {
                        continue;  // Needs a music source that is not configured
                    }
                    if (index >= 1 && i == static_cast<size_t>(index - 1)) {
                        start = tracks.size() - 1;
                    }
                }
                if (tracks.empty()) {
                    return MakeResult(false,
                                      "The favorites need a music source that is not set up");
                }
                cJSON* result = MakeResult(true, "");
                cJSON_AddItemToObject(result, "now_playing", MakeTrackJson(tracks[start]));
                cJSON_AddNumberToObject(result, "queue_length", tracks.size());
                player.SetQueue(std::move(tracks), start, false, "favorites");
                app.PlayMusic(true);
                return result;
            }
            return MakeResult(false, "Unknown action: " + action);
        });
    server.AddTool(std::move(favorites));

    server.AddTool(
        "self.music.get_status",
        "Get the music player status: state, current song, position, play mode and playlist "
        "size. Use it to answer questions such as \"这是什么歌\".",
        PropertyList(), [](const PropertyList&) -> ReturnValue {
            return Application::GetInstance().GetMusicPlayer().GetStatusJson();
        });
}

void AddMusicUserOnlyTools(McpServer& server) {
    auto configure = std::make_unique<McpTool>(
        "self.music.configure_source",
        "Configure the music library used by the music player. The connection is tested before it "
        "is saved.\n"
        "Args:\n"
        "  `type`: `subsonic` (Navidrome, Gonic, Airsonic...), `http` (the JSON API described in "
        "docs/music-player.md) or `none`.\n"
        "  `url`: Server base URL, e.g. http://192.168.1.10:4533\n"
        "  `username`, `password`: Subsonic account. Only a salted token is stored.\n"
        "  `api_key`: Optional bearer token for the `http` type.\n"
        "  `max_bitrate_kbps`: Subsonic transcoding bitrate; 0 streams the original files.",
        PropertyList({Property("type", kPropertyTypeString),
                      Property("url", kPropertyTypeString, std::string("")),
                      Property("username", kPropertyTypeString, std::string("")),
                      Property("password", kPropertyTypeString, std::string("")),
                      Property("api_key", kPropertyTypeString, std::string("")),
                      Property("max_bitrate_kbps", kPropertyTypeInteger, 128, 0, 320)}),
        [](const PropertyList& properties) -> ReturnValue {
            MusicSourceConfig config;
            config.type = properties["type"].value<std::string>();
            auto& player = Application::GetInstance().GetMusicPlayer();
            if (config.type == "none") {
                config.Save();
                player.SetSource(nullptr);
                return MakeResult(true, "Music source removed");
            }

            config.url = properties["url"].value<std::string>();
            while (!config.url.empty() && config.url.back() == '/') {
                config.url.pop_back();
            }
            if (!IsHttpUrl(config.url)) {
                return MakeResult(false, "url must start with http:// or https://");
            }
            config.max_bitrate_kbps = properties["max_bitrate_kbps"].value<int>();
            if (config.type == "subsonic") {
                config.username = properties["username"].value<std::string>();
                const std::string password = properties["password"].value<std::string>();
                if (config.username.empty() || password.empty()) {
                    return MakeResult(false, "username and password are required");
                }
                std::array<uint8_t, 8> salt;
                esp_fill_random(salt.data(), salt.size());
                config.salt = HexEncode(salt.data(), salt.size());
                config.token = SubsonicSource::MakeToken(password, config.salt);
            } else if (config.type == "http") {
                config.api_key = properties["api_key"].value<std::string>();
            } else {
                return MakeResult(false, "type must be subsonic, http or none");
            }

            auto source = CreateMusicSource(config);
            std::string error;
            if (!source || !source->Ping(error)) {
                return MakeResult(false, "Connection test failed: " + error);
            }
            config.Save();
            player.SetSource(source);
            return MakeResult(true, "Music source saved");
        });
    configure->set_user_only(true);
    configure->set_async(true);
    server.AddTool(std::move(configure));

    server.AddUserOnlyTool(
        "self.music.get_source", "Show the configured music source (without secrets).",
        PropertyList(), [](const PropertyList&) -> ReturnValue {
            auto config = MusicSourceConfig::Load();
            cJSON* result = cJSON_CreateObject();
            cJSON_AddStringToObject(result, "type",
                                    config.type.empty() ? "none" : config.type.c_str());
            cJSON_AddStringToObject(result, "url", config.url.c_str());
            cJSON_AddStringToObject(result, "username", config.username.c_str());
            cJSON_AddBoolToObject(result, "has_api_key", !config.api_key.empty());
            cJSON_AddNumberToObject(result, "max_bitrate_kbps", config.max_bitrate_kbps);
            cJSON_AddBoolToObject(
                result, "active",
                Application::GetInstance().GetMusicPlayer().GetSource() != nullptr);
            return result;
        });
}
