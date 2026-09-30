#ifndef POMODORO_H_
#define POMODORO_H_

#include <esp_timer.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "music/music_source.h"

class McpServer;
struct cJSON;

// Focus/break timer controlled by voice through the self.pomodoro.* tools.
//
// Remaining time is derived from an absolute deadline, so it does not drift
// and pausing simply freezes the remaining duration. Optional white noise is
// played through the music player as a looping queue, which gives it the same
// wake-word interruption and resume behavior as music. All state changes run
// in the main task; the public methods are thread-safe.
class Pomodoro {
public:
    enum class Phase { kIdle, kFocus, kBreak };

    static Pomodoro& GetInstance();

    // `white_noise` may be empty. A running pomodoro is replaced.
    void Start(int focus_minutes, int break_minutes, std::vector<MusicTrack> white_noise);
    void Stop();
    void TogglePause();

    bool IsActive() const;
    bool IsPaused() const;
    // Caller owns the returned object.
    cJSON* GetStatusJson() const;

    static void AddTools(McpServer& server);

private:
    Pomodoro();
    Pomodoro(const Pomodoro&) = delete;
    Pomodoro& operator=(const Pomodoro&) = delete;

    mutable std::mutex mutex_;
    Phase phase_ = Phase::kIdle;
    bool paused_ = false;
    int64_t deadline_us_ = 0;
    int64_t paused_remaining_us_ = 0;
    int focus_minutes_ = 25;
    int break_minutes_ = 5;
    bool white_noise_ = false;
    uint32_t generation_ = 0;  // Invalidates timer events from a replaced run
    std::string pending_notice_;
    std::string pending_status_;
    esp_timer_handle_t phase_timer_ = nullptr;
    esp_timer_handle_t display_timer_ = nullptr;
    esp_timer_handle_t notice_timer_ = nullptr;

    int64_t RemainingUsLocked() const;
    void ArmPhaseTimerLocked(int64_t delay_us);
    void StartInMainTask(int focus_minutes, int break_minutes, std::vector<MusicTrack> white_noise);
    void StopInMainTask();
    void TogglePauseInMainTask();
    void HandlePhaseEnd(uint32_t generation);
    void FinishLocked();
    void StopWhiteNoise();
    void RefreshDisplay();
    void ScheduleNotice(const char* status, const char* message);
    void ShowNotice();
};

#endif  // POMODORO_H_
