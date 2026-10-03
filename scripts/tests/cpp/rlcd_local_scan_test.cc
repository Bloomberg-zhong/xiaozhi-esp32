#include <stdlib.h>
#include <unistd.h>
#include <cassert>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <utility>
#include "device_state.h"
#include "esp_log.h"
#include "music/local_music.h"
#define TAG "test"
using BaseType_t = int;
constexpr int pdPASS = 1;
bool fail_task = false;
void (*worker)(void*) = nullptr;
void* worker_arg = nullptr;
int xTaskCreate(void (*f)(void*), const char*, int, void* arg, int, void*) {
    if (fail_task)
        return 0;
    worker = f;
    worker_arg = arg;
    return pdPASS;
}
void RunWorker() {
    auto f = worker;
    worker = nullptr;
    f(worker_arg);
}
int timer_stops = 0;
void esp_timer_stop(void*) { ++timer_stops; }
void vTaskDelete(void*) {}
struct Player {
    std::string root, tag;
    std::vector<MusicTrack> tracks;
    std::string GetLocalRoot() { return root; }
    void SetQueue(std::vector<MusicTrack> value, size_t index, bool loop, std::string value_tag) {
        assert(index == 0 && !loop);
        tracks = std::move(value);
        tag = std::move(value_tag);
    }
};
struct Application {
    Player player;
    DeviceState state = kDeviceStateStarting;
    std::deque<std::function<void()>> tasks;
    int plays = 0;
    static Application& GetInstance() {
        static Application a;
        return a;
    }
    Player& GetMusicPlayer() { return player; }
    DeviceState GetDeviceState() { return state; }
    void Schedule(std::function<void()> f) { tasks.push_back(std::move(f)); }
    void Drain() {
        while (!tasks.empty()) {
            auto f = std::move(tasks.front());
            tasks.pop_front();
            f();
        }
    }
    void PlayMusic(bool restart) {
        assert(restart);
        ++plays;
    }
};
struct Display {
    int notices = 0;
    void ShowNotification(const char*, int) { ++notices; }
};
struct CustomBoard {
    Display display;
    Display* display_ = &display;
    bool local_music_scan_pending_ = false;
    bool local_music_scan_running_ = false;
    uint32_t local_music_scan_revision_ = 0;
    void* connect_timer_ = nullptr;
    // PRODUCTION_METHODS
};
int main() {
    char temp[] = "/tmp/rlcd-local-XXXXXX";
    std::filesystem::path root = mkdtemp(temp);
    std::filesystem::create_directories(root / "儿童音乐");
    std::filesystem::create_directories(root / "white-noise");
    std::filesystem::create_directories(root / ".cache");
    std::ofstream(root / "white-noise/a.mp3") << "noise";
    std::ofstream(root / ".cache/a.mp3") << "cache";
    std::ofstream(root / "readme.txt") << "ignore";
    for (int i = 0; i < 140; ++i)
        std::ofstream(root / "儿童音乐" / (std::to_string(i) + ".mp3")) << "audio";
    CustomBoard board;
    auto& app = Application::GetInstance();
    board.StartLocalMusic();  // missing card doesn't touch queue/timer/network
    assert(!worker && !board.local_music_scan_pending_ && app.plays == 0 && timer_stops == 0);
    app.player.root = root;
    board.StartLocalMusic();
    assert(worker && board.local_music_scan_pending_ && app.player.tracks.empty());
    RunWorker();
    assert(app.player.tracks.empty());  // worker never mutates Application
    app.Drain();
    assert(app.plays == 1 && !board.local_music_scan_pending_ && timer_stops == 1);
    assert(app.player.tracks.size() == 100 && app.player.tag == "sdcard-key");
    for (const auto& track : app.player.tracks) {
        assert(track.stream_url.find("儿童音乐") != std::string::npos);
        assert(IsLocalMusicPath(track.stream_url));
    }
    board.StartLocalMusic();
    ++board.local_music_scan_revision_;
    board.local_music_scan_pending_ = false;
    const auto revision = board.local_music_scan_revision_;
    const auto request = worker_arg;
    board.StartLocalMusic();  // cannot create another worker until the cancelled scan exits
    assert(worker_arg == request && board.local_music_scan_revision_ == revision);
    RunWorker();
    app.Drain();
    assert(app.plays == 1 && timer_stops == 1);
    board.StartLocalMusic();
    app.state = kDeviceStateListening;
    RunWorker();
    app.Drain();
    assert(app.plays == 1);
    app.state = kDeviceStateIdle;
    fail_task = true;
    board.StartLocalMusic();
    assert(!board.local_music_scan_pending_ && app.plays == 1);
    fail_task = false;
    app.player.root = (root / "empty").string();
    std::filesystem::create_directories(app.player.root);
    board.StartLocalMusic();
    RunWorker();
    app.Drain();
    assert(app.plays == 1 && timer_stops == 1 && !board.local_music_scan_pending_);
    std::filesystem::remove_all(root);
}
