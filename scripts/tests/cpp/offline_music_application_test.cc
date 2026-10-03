#include <cassert>
#include <deque>
#include <functional>
#include <memory>
#include <utility>
#include "device_state_machine.h"
#include "esp_log.h"
#include "music/local_music.h"
#define CONFIG_USE_MUSIC_PLAYER 1
#define TAG "test"
#define MAIN_EVENT_NETWORK_CONNECTED 1
enum class PowerSaveLevel { LOW_POWER, PERFORMANCE };
struct Display {
    void UpdateStatusBar(bool) {}
};
struct Board {
    Display display;
    static Board& GetInstance() {
        static Board instance;
        return instance;
    }
    Display* GetDisplay() { return &display; }
    void SetPowerSaveLevel(PowerSaveLevel) {}
};
struct Protocol {
    bool IsAudioChannelOpened() { return false; }
    void CloseAudioChannel() {}
};
struct AudioService {
    bool IsPlaybackIdle() { return true; }
};
struct MusicPlayer {
    MusicTrack track;
    bool wants = false, playing = false, paused = false;
    int starts = 0;
    void Stop() { playing = paused = false; }
    void SetWantsPlayback(bool value) { wants = value; }
    bool WantsPlayback() { return wants; }
    bool HasTrack() { return !track.stream_url.empty(); }
    bool GetCurrentTrack(MusicTrack& out) {
        out = track;
        return HasTrack();
    }
    bool IsPlaying() { return playing; }
    bool IsPaused() { return paused; }
    unsigned Play() {
        ++starts;
        playing = true;
        paused = false;
        return 1;
    }
};
int created_tasks = 0, event_bits = 0;
int xTaskCreate(void (*)(void*), const char*, int, void*, int, void** handle) {
    ++created_tasks;
    *handle = reinterpret_cast<void*>(1);
    return 1;
}
void vTaskDelete(void*) {}
void esp_timer_stop(void*) {}
void xEventGroupSetBits(void*, int bits) { event_bits |= bits; }
struct Application {
    DeviceStateMachine state_machine_;
    MusicPlayer music_player_;
    AudioService audio_service_;
    std::unique_ptr<Protocol> protocol_;
    void* activation_task_handle_ = nullptr;
    void* event_group_ = nullptr;
    void* music_handoff_timer_ = nullptr;
    bool network_connected_ = false, music_handoff_pending_ = false, pending_music_start_ = false;
    int music_failures_ = 0;
    std::deque<std::function<void()>> tasks;
    DeviceState GetDeviceState() { return state_machine_.GetState(); }
    bool SetDeviceState(DeviceState state) { return state_machine_.TransitionTo(state); }
    void Schedule(std::function<void()> callback) { tasks.push_back(std::move(callback)); }
    void Drain() {
        while (!tasks.empty()) {
            auto task = std::move(tasks.front());
            tasks.pop_front();
            task();
        }
    }
    void ActivationTask() {}
    void ArmMusicHandoff() {}
    void StopNotification() {}
    void ShowMusicTrack() {}
    void PlayMusic(bool restart);
    void TryStartMusic();
    void StopMusicPlayback();
    void HandleNetworkConnectedEvent();
    void HandleNetworkDisconnectedEvent();
};
// PRODUCTION_METHODS
int main() {
    for (bool configuring : {false, true}) {
        Application app;
        assert(app.SetDeviceState(kDeviceStateStarting));
        if (configuring)
            assert(app.SetDeviceState(kDeviceStateWifiConfiguring));
        app.music_player_.track.stream_url = "/sdcard/儿童音乐/a.mp3";
        app.PlayMusic(true);
        assert(app.music_player_.starts == 0);  // always scheduled
        app.Drain();
        assert(app.GetDeviceState() == kDeviceStatePlaying);
        assert(app.music_player_.starts == 1);
        created_tasks = event_bits = 0;
        app.HandleNetworkConnectedEvent();  // don't interrupt a local song
        assert(app.network_connected_);
        assert(created_tasks == 0 && app.GetDeviceState() == kDeviceStatePlaying);
        app.music_player_.playing = false;
        app.music_player_.paused = true;
        assert(app.SetDeviceState(kDeviceStateIdle));
        app.HandleNetworkConnectedEvent();  // don't steal the paused player's KEY controls
        assert(created_tasks == 0);
        app.StopMusicPlayback();
        assert(event_bits & MAIN_EVENT_NETWORK_CONNECTED);
        app.HandleNetworkConnectedEvent();
        assert(created_tasks == 1 && app.GetDeviceState() == kDeviceStateActivating);
        app.HandleNetworkDisconnectedEvent();
        assert(!app.network_connected_);
    }
    Application remote;
    assert(remote.SetDeviceState(kDeviceStateStarting));
    remote.music_player_.track.stream_url = "https://server/song.mp3";
    remote.PlayMusic(true);
    remote.Drain();
    assert(remote.GetDeviceState() == kDeviceStateStarting && remote.music_player_.starts == 0);
    Application busy;
    assert(busy.SetDeviceState(kDeviceStateStarting));
    assert(busy.SetDeviceState(kDeviceStateActivating));
    busy.activation_task_handle_ = reinterpret_cast<void*>(1);
    busy.music_player_.track.stream_url = "/sdcard/a.mp3";
    busy.PlayMusic(true);
    busy.Drain();
    assert(busy.GetDeviceState() == kDeviceStateActivating && busy.music_player_.starts == 0);
}
