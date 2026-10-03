#include <cassert>
#include <deque>
#include <functional>
#include <utility>
#include "device_state.h"
#include "esp_log.h"
#define TAG "test"
#define CONFIG_USE_MUSIC_PLAYER 1
#define CONFIG_USE_POMODORO 1
namespace rlcd_dashboard {
enum class DashboardPage { kHome };
}
struct Button {
    std::function<void()> click, twice, triple, hold;
    void OnClick(std::function<void()> f) { click = std::move(f); }
    void OnDoubleClick(std::function<void()> f) { twice = std::move(f); }
    void OnMultipleClick(std::function<void()> f, unsigned) { triple = std::move(f); }
    void OnLongPress(std::function<void()> f) { hold = std::move(f); }
};
struct Player {
    bool playing = false, paused = false;
    bool IsPlaying() { return playing; }
    bool IsPaused() { return paused; }
};
struct Application {
    Player player;
    DeviceState state = kDeviceStateIdle;
    int pauses = 0, resumes = 0, skips = 0, stops = 0;
    std::deque<std::function<void()>> tasks;
    static Application& GetInstance() {
        static Application app;
        return app;
    }
    void Schedule(std::function<void()> f) { tasks.push_back(std::move(f)); }
    void Drain() {
        while (!tasks.empty()) {
            auto f = std::move(tasks.front());
            tasks.pop_front();
            f();
        }
    }
    DeviceState GetDeviceState() { return state; }
    Player& GetMusicPlayer() { return player; }
    void PauseMusic() { ++pauses; }
    void PlayMusic(bool) { ++resumes; }
    void SkipMusic(bool forward) {
        assert(forward);
        ++skips;
    }
    void StopMusic() { ++stops; }
};
struct Pomodoro {
    bool active = false;
    int pauses = 0;
    static Pomodoro& GetInstance() {
        static Pomodoro p;
        return p;
    }
    bool IsActive() { return active; }
    void TogglePause() { ++pauses; }
    void Stop() { active = false; }
};
struct Display {
    int homes = 0;
    int notices = 0;
    void ShowNotification(const char*, int) { ++notices; }
    void RequestPage(rlcd_dashboard::DashboardPage) { ++homes; }
};
struct Power {
    void WakeUp() {}
};
int modes = 0;
void CycleMusicPlayMode() { ++modes; }
struct CustomBoard {
    Button user_button_;
    Display display;
    Display* display_ = &display;
    Power power;
    Power* power_save_timer_ = &power;
    bool local_music_scan_pending_ = false;
    unsigned local_music_scan_revision_ = 0;
    int local_starts = 0;
    void StartLocalMusic() { ++local_starts; }
    // PRODUCTION_METHODS
};
int main() {
    CustomBoard board;
    auto& app = Application::GetInstance();
    board.InitializeUserButton();
    board.user_button_.hold();
    assert(board.local_starts == 0);  // application mutations stay in main task
    app.Drain();
    assert(board.local_starts == 1 && app.stops == 0);
    app.state = kDeviceStateStarting;
    board.user_button_.hold();
    app.Drain();
    assert(board.local_starts == 2);
    app.state = kDeviceStateWifiConfiguring;
    board.user_button_.hold();
    app.Drain();
    assert(board.local_starts == 3);
    app.state = kDeviceStateListening;
    board.user_button_.hold();
    app.Drain();
    assert(board.local_starts == 3);
    app.state = kDeviceStatePlaying;
    app.player.playing = true;
    board.user_button_.click();
    app.Drain();
    assert(app.pauses == 1);
    board.user_button_.twice();
    app.Drain();
    assert(app.skips == 1);
    board.user_button_.triple();
    app.Drain();
    assert(modes == 1);
    board.user_button_.hold();
    app.Drain();
    assert(app.stops >= 1);
    assert(board.local_starts == 3 && board.display.homes >= 1);
    app.state = kDeviceStateIdle;
    app.player.playing = false;
    app.player.paused = true;
    board.user_button_.click();
    app.Drain();
    assert(app.resumes == 1);
    board.user_button_.hold();
    app.Drain();
    assert(board.local_starts == 3);
    app.player.paused = false;
    board.user_button_.click();
    app.Drain();
    board.local_music_scan_pending_ = true;
    unsigned revision = board.local_music_scan_revision_;
    board.user_button_.hold();
    app.Drain();
    assert(board.local_music_scan_revision_ != revision && !board.local_music_scan_pending_);
    assert(board.local_starts == 3);

    // Boot activation must not silently swallow a local-library request.
    app.state = kDeviceStateActivating;
    board.user_button_.hold();
    app.Drain();
    assert(board.local_starts == 4);
    const int stops = app.stops;
    const int notices = board.display.notices;
    app.state = kDeviceStateListening;
    board.user_button_.hold();
    app.Drain();
    assert(board.local_starts == 4 && app.stops == stops);
    assert(board.display.notices == notices + 1);
}
