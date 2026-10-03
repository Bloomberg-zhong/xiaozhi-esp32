"""Run the real channel-close callback against deterministic scheduled events."""

import os
import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]


class MusicHandoffTest(unittest.TestCase):
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
