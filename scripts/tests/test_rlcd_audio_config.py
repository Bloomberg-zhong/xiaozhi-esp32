"""Board wiring regression: the ES7210 MIC3 input is a DAC reference, not a mic."""

import json
import os
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
BOARD = ROOT / "main/boards/waveshare/esp32-s3-rlcd-4.2"


class RlcdAudioConfigTest(unittest.TestCase):
    def test_physical_mic3_reference_gain_is_explicit(self):
        source = (BOARD / "waveshare-s3-rlcd-4.2.cc").read_text()
        method = "virtual AudioCodec* GetAudioCodec() override {" + source.split(
            "virtual AudioCodec* GetAudioCodec() override {", 1
        )[1].split("virtual Display* GetDisplay()", 1)[0]
        driver = r'''
#include <cassert>
#define AUDIO_INPUT_SAMPLE_RATE 24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000
#define AUDIO_I2S_GPIO_MCLK 16
#define AUDIO_I2S_GPIO_BCLK 9
#define AUDIO_I2S_GPIO_WS 45
#define AUDIO_I2S_GPIO_DOUT 8
#define AUDIO_I2S_GPIO_DIN 10
#define AUDIO_CODEC_PA_PIN 46
#define AUDIO_CODEC_ES8311_ADDR 0x18
#define AUDIO_CODEC_ES7210_ADDR 0x40
#define AUDIO_INPUT_REFERENCE true
struct AudioCodec { virtual ~AudioCodec() = default; };
struct BoxAudioCodec : AudioCodec {
    float mic_gain, ref_gain;
    int ref_channel;
    bool reference;
    BoxAudioCodec(void*, int, int, int, int, int, int, int, int, int, int,
                  bool input_reference, float input_gain = 30.0f,
                  int reference_gain_channel = -1, float reference_gain = 0.0f)
        : mic_gain(input_gain), ref_gain(reference_gain),
          ref_channel(reference_gain_channel), reference(input_reference) {}
};
struct Board { virtual AudioCodec* GetAudioCodec() = 0; };
struct RlcdBoard : Board {
    void* i2c_bus_ = nullptr;
'''
        driver += method + "};\n"
        driver += r'''
int main() {
    RlcdBoard board;
    auto* codec = static_cast<BoxAudioCodec*>(board.GetAudioCodec());
    assert(codec->reference);
    assert(codec->mic_gain == 30.0f);
    assert(codec->ref_channel == 2); // physical MIC3, not TDM slot number
    assert(codec->ref_gain == 30.0f);
}
'''
        with tempfile.TemporaryDirectory() as temp:
            cpp = pathlib.Path(temp) / "codec.cc"
            exe = pathlib.Path(temp) / "codec"
            cpp.write_text(driver)
            result = subprocess.run(
                [os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 str(cpp), "-o", str(exe)], capture_output=True, text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


    def test_default_listening_mode_respects_turn_based_policy(self):
        source = (ROOT / "main/application.cc").read_text()
        method = "ListeningMode Application::GetDefaultListeningMode() const {" + source.split(
            "ListeningMode Application::GetDefaultListeningMode() const {", 1
        )[1].split("void Application::Reboot()", 1)[0]
        driver = r'''
#include <cassert>
enum AecMode { kAecOff, kAecOnDeviceSide, kAecOnServerSide };
enum ListeningMode { kListeningModeAutoStop, kListeningModeRealtime };
struct Application {
    AecMode aec_mode_ = kAecOff;
    ListeningMode GetDefaultListeningMode() const;
};
''' + method + r'''
int main() {
    Application app;
    assert(app.GetDefaultListeningMode() == kListeningModeAutoStop);
    app.aec_mode_ = kAecOnDeviceSide;
#if CONFIG_FORCE_AUTO_STOP_LISTENING
    assert(app.GetDefaultListeningMode() == kListeningModeAutoStop);
    app.aec_mode_ = kAecOnServerSide;
    assert(app.GetDefaultListeningMode() == kListeningModeAutoStop);
#else
    assert(app.GetDefaultListeningMode() == kListeningModeRealtime);
    app.aec_mode_ = kAecOnServerSide;
    assert(app.GetDefaultListeningMode() == kListeningModeRealtime);
#endif
}
'''
        with tempfile.TemporaryDirectory() as temp:
            cpp = pathlib.Path(temp) / "listening.cc"
            exe = pathlib.Path(temp) / "listening"
            cpp.write_text(driver)
            for enabled in (0, 1):
                with self.subTest(policy=enabled):
                    result = subprocess.run(
                        [os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                         f"-DCONFIG_FORCE_AUTO_STOP_LISTENING={enabled}", str(cpp), "-o", str(exe)],
                        capture_output=True, text=True,
                    )
                    self.assertEqual(result.returncode, 0, result.stderr)
                    result = subprocess.run([str(exe)], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


    def test_auto_mode_waits_for_real_playback_drain(self):
        source = (ROOT / "main/application.cc").read_text()
        marker = "case kDeviceStateListening:\n            display->SetStatus"
        listening = marker + source.split(marker, 1)[1].split("case kDeviceStateSpeaking:", 1)[0]
        deferred = "if (pending_listening_start_ &&" + source.split(
            "if (pending_listening_start_ &&", 1
        )[1].split("\n        }\n", 1)[0]
        driver = r'''
#include <cassert>
enum DeviceState { kDeviceStateListening, kDeviceStateIdle };
enum ListeningMode { kListeningModeAutoStop, kListeningModeRealtime };
namespace Lang { namespace Strings { const char* LISTENING = "listen"; } }
struct Display { void SetStatus(const char*) {} void SetEmotion(const char*) {} };
struct AudioService {
    bool idle = false, running = false;
    bool IsPlaybackIdle() { return idle; }
    bool IsAudioProcessorRunning() { return running; }
};
struct Application {
    AudioService audio_service_;
    Display ui;
    DeviceState state = kDeviceStateListening;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    bool play_popup_on_listening_ = false, pending_listening_start_ = false;
    int starts = 0;
    DeviceState GetDeviceState() { return state; }
    void StartListeningAudio() { ++starts; audio_service_.running = true; }
    void ConfigureWakeWordForListening() {}
    void EnterListening() {
        auto* display = &ui;
        switch (state) {
''' + listening + r'''
        default: break;
        }
    }
    void OnPlaybackDrained() {
''' + deferred + r'''
    }
};
int main() {
    Application app;
    app.EnterListening();
    assert(app.pending_listening_start_);
    assert(app.starts == 0);
    app.OnPlaybackDrained(); // stale drain notification while output still active
    assert(app.starts == 0);
    app.audio_service_.idle = true;
    app.OnPlaybackDrained();
    assert(app.starts == 1);
    assert(!app.pending_listening_start_);
    app.OnPlaybackDrained();
    assert(app.starts == 1);
    app.pending_listening_start_ = true;
    app.state = kDeviceStateIdle; // conversation was closed before drain
    app.OnPlaybackDrained();
    assert(app.starts == 1);
}
'''
        with tempfile.TemporaryDirectory() as temp:
            cpp = pathlib.Path(temp) / "drain.cc"
            exe = pathlib.Path(temp) / "drain"
            cpp.write_text(driver)
            result = subprocess.run(
                [os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 str(cpp), "-o", str(exe)], capture_output=True, text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_turn_based_aec_preserves_near_end_wake_speech(self):
        source = (ROOT / "main/audio/engines/afe_audio_engine.cc").read_text()
        assignment = source.split("afe_config->aec_init = codec_->input_reference();", 1)[1].split(
            "afe_config->ns_init", 1
        )[0]
        driver = r'''
#include <cassert>
enum Level { AEC_NLP_LEVEL_NORMAL, AEC_NLP_LEVEL_VERYAGGR };
enum Aec { AEC_MODE_SR_LOW_COST, AEC_MODE_FD_LOW_COST };
struct Config { Level aec_nlp_level; Aec aec_mode; };
int main() {
    Config config;
    auto* afe_config = &config;
''' + assignment + r'''
#if CONFIG_FORCE_AUTO_STOP_LISTENING
    assert(config.aec_nlp_level == AEC_NLP_LEVEL_NORMAL);
    assert(config.aec_mode == AEC_MODE_SR_LOW_COST);
#else
    assert(config.aec_nlp_level == AEC_NLP_LEVEL_VERYAGGR);
    assert(config.aec_mode == AEC_MODE_FD_LOW_COST);
#endif
}
'''
        with tempfile.TemporaryDirectory() as temp:
            cpp = pathlib.Path(temp) / "aec.cc"
            exe = pathlib.Path(temp) / "aec"
            cpp.write_text(driver)
            for enabled in (0, 1):
                with self.subTest(policy=enabled):
                    result = subprocess.run(
                        [os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                         f"-DCONFIG_FORCE_AUTO_STOP_LISTENING={enabled}", str(cpp), "-o", str(exe)],
                        capture_output=True, text=True,
                    )
                    self.assertEqual(result.returncode, 0, result.stderr)
                    result = subprocess.run([str(exe)], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_conversation_keeps_the_official_realtime_interruption(self):
        """Voice uplink must stay on while the assistant speaks, otherwise the user
        can only interrupt with the wake word, and the weaker AEC that mode needs
        makes the wake word less reliable while music plays."""
        config = json.loads((BOARD / "config.json").read_text())
        for build in config["builds"]:
            with self.subTest(variant=build["name"]):
                options = build["sdkconfig_append"]
                self.assertIn("CONFIG_USE_DEVICE_AEC=y", options)
                self.assertIn("CONFIG_WAKE_WORD_DETECTION_IN_LISTENING=y", options)
                self.assertNotIn("CONFIG_FORCE_AUTO_STOP_LISTENING=y", options)


if __name__ == "__main__":
    unittest.main()
