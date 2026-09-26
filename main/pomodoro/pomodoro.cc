#include "pomodoro.h"

#include <esp_log.h>
#include <esp_random.h>
#include <cJSON.h>

#include <memory>

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "display.h"
#include "mcp_server.h"
#if CONFIG_USE_MUSIC_PLAYER
#include "music/local_music.h"
#endif

#define TAG "Pomodoro"

namespace {
constexpr int64_t kUsPerMinute = 60LL * 1000 * 1000;
// The idle clock replaces the status text after 10 s without updates.
constexpr int64_t kDisplayRefreshUs = 5LL * 1000 * 1000;
// Lets the white noise stop (and the idle screen settle) before the notice.
constexpr int64_t kNoticeDelayUs = 600LL * 1000;
constexpr const char* kWhiteNoiseQueueTag = "pomodoro";
}  // namespace

Pomodoro& Pomodoro::GetInstance() {
    static Pomodoro instance;
    return instance;
}

Pomodoro::Pomodoro() {
    auto create = [this](esp_timer_cb_t callback, const char* name, esp_timer_handle_t* handle) {
        esp_timer_create_args_t args = {
            .callback = callback,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = name,
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&args, handle));
    };
    create(
        [](void* arg) {
            auto* self = static_cast<Pomodoro*>(arg);
            uint32_t generation;
            {
                std::lock_guard<std::mutex> lock(self->mutex_);
                generation = self->generation_;
            }
            Application::GetInstance().Schedule(
                [self, generation]() { self->HandlePhaseEnd(generation); });
        },
        "pomodoro_phase", &phase_timer_);
    create(
        [](void* arg) {
            auto* self = static_cast<Pomodoro*>(arg);
            Application::GetInstance().Schedule([self]() { self->RefreshDisplay(); });
        },
        "pomodoro_display", &display_timer_);
    create(
        [](void* arg) {
            auto* self = static_cast<Pomodoro*>(arg);
            Application::GetInstance().Schedule([self]() { self->ShowNotice(); });
        },
        "pomodoro_notice", &notice_timer_);
}

void Pomodoro::Start(int focus_minutes, int break_minutes, std::vector<MusicTrack> white_noise) {
    Application::GetInstance().Schedule(
        [this, focus_minutes, break_minutes, white_noise = std::move(white_noise)]() mutable {
            StartInMainTask(focus_minutes, break_minutes, std::move(white_noise));
        });
}

void Pomodoro::Stop() {
    Application::GetInstance().Schedule([this]() { StopInMainTask(); });
}

void Pomodoro::TogglePause() {
    Application::GetInstance().Schedule([this]() { TogglePauseInMainTask(); });
}

bool Pomodoro::IsActive() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return phase_ != Phase::kIdle;
}

int64_t Pomodoro::RemainingUsLocked() const {
    if (phase_ == Phase::kIdle) {
        return 0;
    }
    if (paused_) {
        return paused_remaining_us_;
    }
    int64_t remaining = deadline_us_ - esp_timer_get_time();
    return remaining > 0 ? remaining : 0;
}

void Pomodoro::ArmPhaseTimerLocked(int64_t delay_us) {
    esp_timer_stop(phase_timer_);
    esp_timer_start_once(phase_timer_, delay_us > 0 ? delay_us : 1);
}

cJSON* Pomodoro::GetStatusJson() const {
    std::lock_guard<std::mutex> lock(mutex_);
    cJSON* root = cJSON_CreateObject();
    const char* phase = phase_ == Phase::kFocus   ? "focus"
                        : phase_ == Phase::kBreak ? "break"
                                                  : "idle";
    cJSON_AddStringToObject(root, "phase", phase);
    cJSON_AddBoolToObject(root, "paused", paused_);
    cJSON_AddNumberToObject(root, "remaining_seconds", RemainingUsLocked() / 1000000);
    cJSON_AddNumberToObject(root, "focus_minutes", focus_minutes_);
    cJSON_AddNumberToObject(root, "break_minutes", break_minutes_);
    cJSON_AddBoolToObject(root, "white_noise", white_noise_);
    return root;
}

void Pomodoro::StartInMainTask(int focus_minutes, int break_minutes,
                               std::vector<MusicTrack> white_noise) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++generation_;
        phase_ = Phase::kFocus;
        paused_ = false;
        focus_minutes_ = focus_minutes;
        break_minutes_ = break_minutes;
        deadline_us_ = esp_timer_get_time() + focus_minutes * kUsPerMinute;
        ArmPhaseTimerLocked(focus_minutes * kUsPerMinute);
        esp_timer_stop(display_timer_);
        esp_timer_start_periodic(display_timer_, kDisplayRefreshUs);
        white_noise_ = false;
    }
    ESP_LOGI(TAG, "Focus %d min, break %d min, %u white noise files", focus_minutes, break_minutes,
             static_cast<unsigned>(white_noise.size()));

#if CONFIG_USE_MUSIC_PLAYER
    if (!white_noise.empty()) {
        auto& app = Application::GetInstance();
        size_t start = esp_random() % white_noise.size();
        app.GetMusicPlayer().SetQueue(std::move(white_noise), start, true, kWhiteNoiseQueueTag);
        app.PlayMusic(true);
        std::lock_guard<std::mutex> lock(mutex_);
        white_noise_ = true;
    }
#endif
    RefreshDisplay();
}

void Pomodoro::StopWhiteNoise() {
#if CONFIG_USE_MUSIC_PLAYER
    bool playing;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        playing = white_noise_;
        white_noise_ = false;
    }
    // Leave the player alone if the user switched to other music meanwhile.
    auto& app = Application::GetInstance();
    if (playing && app.GetMusicPlayer().queue_tag() == kWhiteNoiseQueueTag) {
        app.StopMusic();
    }
#endif
}

void Pomodoro::FinishLocked() {
    phase_ = Phase::kIdle;
    paused_ = false;
    esp_timer_stop(phase_timer_);
    esp_timer_stop(display_timer_);
}

void Pomodoro::StopInMainTask() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (phase_ == Phase::kIdle) {
            return;
        }
        ++generation_;
        FinishLocked();
    }
    StopWhiteNoise();
    if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
        Board::GetInstance().GetDisplay()->SetStatus(Lang::Strings::STANDBY);
    }
}

void Pomodoro::TogglePauseInMainTask() {
    bool paused;
    bool white_noise;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (phase_ == Phase::kIdle) {
            return;
        }
        if (!paused_) {
            paused_remaining_us_ = RemainingUsLocked();
            paused_ = true;
            esp_timer_stop(phase_timer_);
        } else {
            deadline_us_ = esp_timer_get_time() + paused_remaining_us_;
            paused_ = false;
            ArmPhaseTimerLocked(paused_remaining_us_);
        }
        paused = paused_;
        white_noise = white_noise_ && phase_ == Phase::kFocus;
    }
#if CONFIG_USE_MUSIC_PLAYER
    auto& app = Application::GetInstance();
    if (white_noise && app.GetMusicPlayer().queue_tag() == kWhiteNoiseQueueTag) {
        if (paused) {
            app.PauseMusic();
        } else {
            app.PlayMusic(false);
        }
    }
#else
    (void)white_noise;
#endif
    RefreshDisplay();
}

void Pomodoro::HandlePhaseEnd(uint32_t generation) {
    Phase ended;
    int break_minutes;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation != generation_ || phase_ == Phase::kIdle || paused_) {
            return;
        }
        ended = phase_;
        break_minutes = break_minutes_;
        if (ended == Phase::kFocus && break_minutes > 0) {
            phase_ = Phase::kBreak;
            deadline_us_ = esp_timer_get_time() + break_minutes * kUsPerMinute;
            ArmPhaseTimerLocked(break_minutes * kUsPerMinute);
        } else {
            FinishLocked();
        }
    }

    if (ended == Phase::kFocus) {
        StopWhiteNoise();
        if (break_minutes > 0) {
            ScheduleNotice(Lang::Strings::POMODORO_BREAK, Lang::Strings::POMODORO_FOCUS_DONE);
        } else {
            ScheduleNotice(Lang::Strings::POMODORO_FOCUS, Lang::Strings::POMODORO_DONE);
        }
    } else {
        ScheduleNotice(Lang::Strings::POMODORO_BREAK, Lang::Strings::POMODORO_BREAK_DONE);
    }
    ESP_LOGI(TAG, "%s finished", ended == Phase::kFocus ? "Focus" : "Break");
}

void Pomodoro::ScheduleNotice(const char* status, const char* message) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_status_ = status;
        pending_notice_ = message;
    }
    esp_timer_stop(notice_timer_);
    esp_timer_start_once(notice_timer_, kNoticeDelayUs);
}

void Pomodoro::ShowNotice() {
    std::string status;
    std::string message;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status = std::move(pending_status_);
        message = std::move(pending_notice_);
    }
    if (message.empty()) {
        return;
    }
    auto& app = Application::GetInstance();
    auto& board = Board::GetInstance();
    auto state = app.GetDeviceState();
    if (state == kDeviceStateIdle || state == kDeviceStatePlaying) {
        // Raising the level wakes boards from their power save mode first.
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        app.Alert(status.c_str(), message.c_str(), "happy", Lang::Sounds::OGG_VIBRATION);
        if (state == kDeviceStateIdle) {
            board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        }
    } else {
        // Do not talk over a conversation; only show it.
        board.GetDisplay()->ShowNotification(message, 5000);
    }
}

void Pomodoro::RefreshDisplay() {
    std::string text;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (phase_ == Phase::kIdle) {
            return;
        }
        int64_t minutes = (RemainingUsLocked() + kUsPerMinute - 1) / kUsPerMinute;
        if (minutes < 1) {
            minutes = 1;
        }
        text = paused_                   ? Lang::Strings::POMODORO_PAUSED
               : phase_ == Phase::kFocus ? Lang::Strings::POMODORO_FOCUS
                                         : Lang::Strings::POMODORO_BREAK;
        text += " " + std::to_string(minutes) + Lang::Strings::POMODORO_MINUTES_LEFT;
    }
    // Conversations own the status bar; show the timer only while idle or playing.
    auto state = Application::GetInstance().GetDeviceState();
    if (state == kDeviceStateIdle || state == kDeviceStatePlaying) {
        Board::GetInstance().GetDisplay()->SetStatus(text.c_str());
    }
}

void Pomodoro::AddTools(McpServer& server) {
    auto start = std::make_unique<McpTool>(
        "self.pomodoro.start",
        "Start a pomodoro timer: a focus countdown, optionally followed by a break countdown. "
        "White noise from the SD card folder `white-noise` can play during the focus time. A "
        "running pomodoro is replaced.\n"
        "Args:\n"
        "  `focus_min`: Focus minutes (default 25).\n"
        "  `break_min`: Break minutes after the focus time, 0 for no break (default 5).\n"
        "  `white_noise`: Play white noise while focusing (default true).",
        PropertyList({Property("focus_min", kPropertyTypeInteger, 25, 1, 120),
                      Property("break_min", kPropertyTypeInteger, 5, 0, 30),
                      Property("white_noise", kPropertyTypeBoolean, true)}),
        [](const PropertyList& properties) -> ReturnValue {
            const int focus = properties["focus_min"].value<int>();
            const int rest = properties["break_min"].value<int>();
            const bool want_noise = properties["white_noise"].value<bool>();
            std::vector<MusicTrack> noise;
            std::string message;
#if CONFIG_USE_MUSIC_PLAYER
            if (want_noise) {
                std::string root = Application::GetInstance().GetMusicPlayer().GetLocalRoot();
                if (root.empty()) {
                    message = "No SD card, white noise is unavailable";
                } else {
                    noise = ScanLocalMusic(root + "/" + kWhiteNoiseFolder);
                    if (noise.empty()) {
                        message = "No audio files in the SD card folder white-noise";
                    }
                }
            }
#else
            if (want_noise) {
                message = "White noise needs the music player";
            }
#endif
            const bool has_noise = !noise.empty();
            Pomodoro::GetInstance().Start(focus, rest, std::move(noise));
            cJSON* result = cJSON_CreateObject();
            cJSON_AddBoolToObject(result, "success", true);
            cJSON_AddNumberToObject(result, "focus_minutes", focus);
            cJSON_AddNumberToObject(result, "break_minutes", rest);
            cJSON_AddBoolToObject(result, "white_noise", has_noise);
            if (!message.empty()) {
                cJSON_AddStringToObject(result, "message", message.c_str());
            }
            return result;
        });
    // Scanning the SD card is file I/O: keep it off the main loop.
    start->set_async(true);
    server.AddTool(std::move(start));

    server.AddTool("self.pomodoro.stop", "Stop the pomodoro timer and its white noise.",
                   PropertyList(), [](const PropertyList&) -> ReturnValue {
                       Pomodoro::GetInstance().Stop();
                       return true;
                   });

    server.AddTool("self.pomodoro.pause",
                   "Pause the pomodoro timer, or resume it when it is paused.", PropertyList(),
                   [](const PropertyList&) -> ReturnValue {
                       if (!Pomodoro::GetInstance().IsActive()) {
                           return std::string("No pomodoro is running");
                       }
                       Pomodoro::GetInstance().TogglePause();
                       return true;
                   });

    server.AddTool("self.pomodoro.status",
                   "Get the pomodoro state: phase (focus, break or idle), paused, remaining "
                   "seconds and settings. Use it for questions like \"还剩多少时间\".",
                   PropertyList(), [](const PropertyList&) -> ReturnValue {
                       return Pomodoro::GetInstance().GetStatusJson();
                   });
}
