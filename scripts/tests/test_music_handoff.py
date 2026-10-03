"""Run the real channel-close callback against deterministic scheduled events."""

import os
import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]


class MusicHandoffTest(unittest.TestCase):
    def test_music_wake_acknowledges_after_pause_before_opening_channel(self):
        source = (ROOT / "main/application.cc").read_text()
        callback = "void Application::HandleWakeWordDetectedEvent()" + source.split(
            "void Application::HandleWakeWordDetectedEvent()", 1
        )[1].split("void Application::BeginWakeWordInvoke", 1)[0]
        driver = r'''
#include <atomic>
#include <cassert>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include "device_state.h"
#define CONFIG_USE_MUSIC_PLAYER 1
#define ESP_LOGI(...) ((void)0)
#define TAG "test"
enum { kAbortReasonWakeWordDetected };
int64_t esp_timer_get_time() { return 1000; }
namespace Lang::Sounds { constexpr std::string_view OGG_POPUP = "popup"; }
std::vector<std::string> actions;
struct Audio {
    std::string GetLastWakeWord() { return "你好小智"; }
    void EnableWakeWordDetection(bool) { actions.push_back("enable_wake"); }
    std::unique_ptr<int> PopPacketFromSendQueue() { return nullptr; }
    void ResetDecoder() { actions.push_back("reset"); }
    void PlaySound(std::string_view sound) { assert(sound == "popup"); actions.push_back("popup"); }
};
struct Protocol { void SendStartListening(int) {} };
struct Application {
    std::unique_ptr<Protocol> protocol_ = std::make_unique<Protocol>();
    Audio audio_service_;
    DeviceState state = kDeviceStatePlaying;
    std::atomic<int64_t> music_wake_started_us_{0};
    bool play_popup_on_listening_ = false;
    DeviceState GetDeviceState() { return state; }
    void SetDeviceState(DeviceState s) { state = s; }
    void SuspendMusicForChat() { actions.push_back("pause"); audio_service_.ResetDecoder(); state = kDeviceStateIdle; }
    void BeginWakeWordInvoke(const std::string&) { assert(state == kDeviceStateIdle); actions.push_back("connect"); }
    void StopNotification() { state = kDeviceStateIdle; }
    void AbortSpeaking(int) {}
    int GetDefaultListeningMode() { return 0; }
    void SetListeningMode(int) {}
    void HandleWakeWordDetectedEvent();
};
'''
        driver += callback + r'''
int main() {
    Application app;
    app.HandleWakeWordDetectedEvent();
#if CONFIG_SEND_WAKE_WORD_DATA
    assert((actions == std::vector<std::string>{"pause", "reset", "popup", "connect"}));
#else
    assert((actions == std::vector<std::string>{"pause", "reset", "connect"}));
#endif
    actions.clear();
    app.state = kDeviceStateIdle;
    app.HandleWakeWordDetectedEvent();
    assert((actions == std::vector<std::string>{"connect"}));
    actions.clear();
    app.protocol_.reset();
    app.state = kDeviceStatePlaying;
    app.HandleWakeWordDetectedEvent();
    assert((actions == std::vector<std::string>{"enable_wake"}));
}
'''
        with tempfile.TemporaryDirectory() as temp:
            cpp = pathlib.Path(temp) / "wake.cc"
            exe = pathlib.Path(temp) / "wake"
            cpp.write_text(driver)
            for send_wake_data in (0, 1):
                with self.subTest(send_wake_data=send_wake_data):
                    result = subprocess.run(
                        [os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                         f"-DCONFIG_SEND_WAKE_WORD_DATA={send_wake_data}",
                         "-I" + str(ROOT / "main"), str(cpp), "-o", str(exe)],
                        capture_output=True, text=True,
                    )
                    self.assertEqual(result.returncode, 0, result.stderr)
                    result = subprocess.run([str(exe)], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_wake_reply_keeps_conversation_open_until_explicit_music_request(self):
        source = (ROOT / "main/application.cc").read_text()
        callback = source.split(
            '} else if (strcmp(state->valuestring, "stop") == 0) {', 1
        )[1].split('} else if (strcmp(state->valuestring, "sentence_start")', 1)[0]
        driver = r'''
#include <cassert>
#include <functional>
#include <memory>
#include <utility>
#include <vector>
#define CONFIG_USE_MUSIC_PLAYER 1
enum DeviceState { kDeviceStateIdle, kDeviceStatePlaying, kDeviceStateSpeaking, kDeviceStateListening };
enum ListeningMode { kListeningModeManualStop, kListeningModeAutoStop };
int esp_timer_stop(void*) { return 0; }
struct Protocol { int closes = 0; void CloseAudioChannel() { ++closes; } };
struct MusicPlayer {
    bool wants = true, has_track = true;
    bool WantsPlayback() { return wants; }
    bool HasTrack() { return has_track; }
};
struct Application {
    std::unique_ptr<Protocol> protocol_ = std::make_unique<Protocol>();
    MusicPlayer music_player_;
    bool music_handoff_pending_ = false;
    void* music_handoff_timer_ = nullptr;
    DeviceState state = kDeviceStateSpeaking;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    std::vector<std::function<void()>> tasks;
    DeviceState GetDeviceState() { return state; }
    void SetDeviceState(DeviceState next) { state = next; }
    void Schedule(std::function<void()> callback) { tasks.push_back(std::move(callback)); }
    void Drain() { for (auto& task : tasks) task(); tasks.clear(); }
    void OnTtsStop() {
'''
        driver += callback + "}\n};\n"
        driver += r'''
int main() {
    Application app;
    app.OnTtsStop(); // greeting after waking during music: keep listening
    app.Drain();
    assert(app.state == kDeviceStateListening);
    assert(app.protocol_->closes == 0);
    app.state = kDeviceStateSpeaking;
    app.music_handoff_pending_ = true; // explicit play/next request this conversation
    app.OnTtsStop();
    app.Drain();
    assert(app.state == kDeviceStateIdle);
    assert(app.protocol_->closes == 1);
    assert(!app.music_handoff_pending_);
    app.state = kDeviceStateSpeaking;
    app.listening_mode_ = kListeningModeManualStop;
    app.OnTtsStop();
    app.Drain();
    assert(app.state == kDeviceStateIdle);
    assert(app.protocol_->closes == 1);
    app.state = kDeviceStateSpeaking;
    app.listening_mode_ = kListeningModeAutoStop;
    app.music_handoff_pending_ = true;
    app.music_player_.has_track = false;
    app.OnTtsStop();
    app.Drain();
    assert(app.state == kDeviceStateListening);
    assert(app.protocol_->closes == 1);
}
'''
        with tempfile.TemporaryDirectory() as temp:
            cpp = pathlib.Path(temp) / "reply.cc"
            exe = pathlib.Path(temp) / "reply"
            cpp.write_text(driver)
            result = subprocess.run(
                [os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 str(cpp), "-o", str(exe)], capture_output=True, text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_late_channel_close_preserves_music_power(self):
        source = (ROOT / "main/application.cc").read_text()
        callback = "protocol_->OnAudioChannelClosed(" + source.split(
            "protocol_->OnAudioChannelClosed(", 1
        )[1].split("protocol_->OnIncomingJson", 1)[0]
        driver = r'''
#include <cassert>
#include <functional>
#include <memory>
#include <utility>
#include <vector>
#define CONFIG_USE_MUSIC_PLAYER 1
enum DeviceState { kDeviceStateIdle, kDeviceStatePlaying };
enum class PowerSaveLevel { LOW_POWER, PERFORMANCE };
struct Display { int clears = 0; void SetChatMessage(const char*, const char*) { ++clears; } };
struct Board {
    PowerSaveLevel power = PowerSaveLevel::PERFORMANCE;
    Display display;
    static Board& GetInstance() { static Board board; return board; }
    void SetPowerSaveLevel(PowerSaveLevel level) { power = level; }
    Display* GetDisplay() { return &display; }
};
struct Protocol {
    std::function<void()> close;
    void OnAudioChannelClosed(std::function<void()> callback) { close = std::move(callback); }
};
struct Application {
    std::unique_ptr<Protocol> protocol_ = std::make_unique<Protocol>();
    DeviceState state = kDeviceStatePlaying;
    bool wants_music = false;
    std::vector<std::function<void()>> tasks;
    DeviceState GetDeviceState() { return state; }
    void SetDeviceState(DeviceState next) { state = next; }
    void Schedule(std::function<void()> callback) { tasks.push_back(std::move(callback)); }
    void TryStartMusic() {
        if (wants_music) {
            state = kDeviceStatePlaying;
            Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        }
    }
    void Drain() { for (auto& task : tasks) task(); tasks.clear(); }
    void Initialize() {
        auto& board = Board::GetInstance();
        (void)board;
'''
        driver += callback + "}\n};\n"
        driver += r'''
int main() {
    auto& board = Board::GetInstance();
    Application app;
    app.Initialize();
    app.protocol_->close(); // goodbye arrives after music has resumed
    assert(board.power == PowerSaveLevel::PERFORMANCE);
    app.Drain();
    assert(app.state == kDeviceStatePlaying);
    assert(board.power == PowerSaveLevel::PERFORMANCE);
    assert(board.display.clears == 0);

    app.state = kDeviceStateIdle;
    app.protocol_->close(); // close is posted, then another event starts music
    app.state = kDeviceStatePlaying;
    app.Drain();
    assert(board.power == PowerSaveLevel::PERFORMANCE);
    assert(board.display.clears == 0);

    app.state = kDeviceStateIdle;
    app.protocol_->close();
    app.Drain();
    assert(board.power == PowerSaveLevel::LOW_POWER);
    assert(board.display.clears == 1);

    app.wants_music = true;
    app.protocol_->close();
    app.Drain();
    assert(app.state == kDeviceStatePlaying);
    assert(board.power == PowerSaveLevel::PERFORMANCE);
}
'''
        with tempfile.TemporaryDirectory() as temp:
            cpp = pathlib.Path(temp) / "handoff.cc"
            exe = pathlib.Path(temp) / "handoff"
            cpp.write_text(driver)
            result = subprocess.run(
                [os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 str(cpp), "-o", str(exe)], capture_output=True, text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
