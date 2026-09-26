#include "music_tools.h"

#include <esp_random.h>

#include <array>
#include <memory>

#include "application.h"
#include "mcp_server.h"
#include "music_player.h"
#include "music_source.h"
#include "music_util.h"
#include "settings.h"
#include "subsonic_source.h"

namespace {

constexpr int kSearchLimit = 20;
constexpr const char* kSettingsNamespace = "music";

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
    cJSON_AddStringToObject(item, "album", track.album.c_str());
    return item;
}

}  // namespace

void LoadMusicSettings(MusicPlayer& player) {
    player.SetSource(CreateMusicSource(MusicSourceConfig::Load()));
    Settings settings(kSettingsNamespace, false);
    MusicPlayMode mode;
    if (ParseMusicPlayMode(settings.GetString("mode", "sequence"), mode)) {
        player.SetPlayMode(mode);
    }
}

void AddMusicTools(McpServer& server) {
    auto play = std::make_unique<McpTool>(
        "self.music.play",
        "Search the user's music library and play the songs found. Use it whenever the user asks "
        "to play music, a song, a singer or an album (for example \"播放周杰伦的稻香\" or "
        "\"来点音乐\").\n"
        "Args:\n"
        "  `query`: Song title and/or artist keywords, e.g. \"稻香 周杰伦\". Use an empty string "
        "for random songs.\n"
        "Return:\n"
        "  The first song and the queue length. Playback starts right after your reply, so keep "
        "the reply short (e.g. \"好的，为你播放稻香\").",
        PropertyList({Property("query", kPropertyTypeString, std::string(""))}),
        [](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            auto& player = app.GetMusicPlayer();
            auto source = player.GetSource();
            if (!source) {
                return MakeResult(false,
                                  "No music source is configured. The user must configure one in "
                                  "the device console (self.music.configure_source).");
            }
            std::vector<MusicTrack> tracks;
            std::string error;
            if (!source->Search(properties["query"].value<std::string>(), kSearchLimit, tracks,
                                error)) {
                return MakeResult(false, error);
            }
            cJSON* result = MakeResult(true, "");
            cJSON_AddItemToObject(result, "now_playing", MakeTrackJson(tracks.front()));
            cJSON_AddNumberToObject(result, "queue_length", tracks.size());
            player.SetQueue(std::move(tracks), 0);
            app.PlayMusic(true);
            return result;
        });
    play->set_async(true);
    server.AddTool(std::move(play));

    server.AddTool(
        "self.music.control",
        "Control music playback.\n"
        "Args:\n"
        "  `action`: `pause` (keep the position), `resume`, `next`, `previous` or `stop`.",
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
        "Set how the music queue continues.\n"
        "Args:\n"
        "  `mode`: `sequence` (stop at the end), `repeat_all`, `repeat_one` or `shuffle`.",
        PropertyList({Property("mode", kPropertyTypeString)}),
        [](const PropertyList& properties) -> ReturnValue {
            MusicPlayMode mode;
            const std::string name = properties["mode"].value<std::string>();
            if (!ParseMusicPlayMode(name, mode)) {
                return MakeResult(false, "Unknown mode: " + name);
            }
            Application::GetInstance().GetMusicPlayer().SetPlayMode(mode);
            Settings settings(kSettingsNamespace, true);
            settings.SetString("mode", name);
            return MakeResult(true, "");
        });

    server.AddTool(
        "self.music.get_status",
        "Get the music player status: state, current song, position, play mode and queue. Use it "
        "to answer questions such as \"这是什么歌\".",
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
