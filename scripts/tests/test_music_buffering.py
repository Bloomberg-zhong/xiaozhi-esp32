"""Run the production decoder loop against deterministic network/codec boundaries.

The codec double turns each input chunk into one identifiable PCM frame. The
reader pauses after the initial reserve, then delivers small bursts; assertions
observe when DecodeTask consumes those bursts and whether parser/PCM state is
preserved. No wall-clock sleep or real ESP hardware is needed.
"""

import os
import pathlib
import re
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


class MusicBufferingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = (ROOT / "main/music/music_player.cc").read_text()
        decode = "void MusicPlayer::DecodeTask(" + source.split(
            "void MusicPlayer::DecodeTask(", 1
        )[1]
        helper = ""
        if "bool MusicPlayer::WaitForBuffer(" in source:
            helper = "bool MusicPlayer::WaitForBuffer(" + source.split(
                "bool MusicPlayer::WaitForBuffer(", 1
            )[1].split("void MusicPlayer::DecodeTask(", 1)[0]
        constants = "\n".join(
            declaration for declaration, name in re.findall(
                r"(constexpr (?:size_t|int) (k\w+) = [^;]+;)", source
            ) if re.search(r"\b" + name + r"\b", helper + decode)
        )
        driver = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define TAG "test"
#define pdMS_TO_TICKS(x) (x)
#define portTICK_PERIOD_MS 1
using TickType_t = uint32_t;
struct Buffer { std::deque<uint8_t> bytes; };
struct Session {
    Buffer storage;
    Buffer* buffer = &storage;
    size_t buffer_capacity = 262143;
    std::atomic<bool> local_stream{false};
    bool live_stream = false;
    struct { bool live = false; std::string stream_url = "http://song"; } track;
    std::atomic<bool> cancelled{false}, paused{false}, net_done{false}, net_failed{false};
    bool decode_done = false;
    std::mutex mutex;
    std::string content_type = "audio/mpeg", error = "network failed";
};
static Session* active;
static int mode, phase, delays, output_frames, process_calls, empty_receives;
static uint32_t ticks;
static size_t first_read, recovery_read, received_total;
static std::vector<uint32_t> positions;
static bool partial_parser, parser_waited, paused_once, cancelled_while_paused;
static std::string failure;
TickType_t xTaskGetTickCount() { return ticks; }
size_t xStreamBufferBytesAvailable(Buffer* buffer) { return buffer->bytes.size(); }
bool xStreamBufferIsEmpty(Buffer* buffer) { return buffer->bytes.empty(); }
void Feed(size_t count) {
    count = std::min(count, active->buffer_capacity - active->storage.bytes.size());
    while (count--) active->storage.bytes.push_back(0x5a);
}
void Advance(int ms) {
    ticks += ms;
    assert(++delays < 2000); // A threshold above capacity/EOF must not wait forever.
    if (mode == 5) { active->cancelled = true; return; }
    if (mode == 6) { active->net_failed = true; return; }
    if (phase == 1) {
        if ((mode == 7 || mode == 8) && !paused_once) {
            active->paused = true;
            paused_once = true;
            return;
        }
        if (active->paused) {
            if (mode == 8) {
                active->cancelled = true;
                cancelled_while_paused = true;
                return;
            }
            active->paused = false;
        }
        if (mode == 9) { active->net_done = true; return; }
        if (mode == 10) { active->net_failed = true; return; }
    }
    Feed(2048);
}
void vTaskDelay(int ms) { Advance(ms); }
size_t xStreamBufferReceive(Buffer* buffer, void* data, size_t capacity, int) {
    if (phase == 1 && buffer->bytes.empty() && ++empty_receives > 1) Advance(50);
    if (buffer->bytes.empty()) return 0;
    if (!first_read) {
        first_read = buffer->bytes.size();
        if (mode == 0) assert(first_read >= 49152);
    }
    if (phase == 1 && !recovery_read) {
        recovery_read = buffer->bytes.size();
        if (mode == 2 || mode == 7 || mode == 12) assert(recovery_read >= 32768);
    }
    size_t size = std::min(capacity, buffer->bytes.size());
    auto* bytes = static_cast<uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) { bytes[i] = buffer->bytes.front(); buffer->bytes.pop_front(); }
    received_total += size;
    return size;
}
struct Codec { int output_sample_rate() { return 1000; } };
struct Board {
    static Board& GetInstance() { static Board board; return board; }
    Codec* GetAudioCodec() { static Codec codec; return &codec; }
};
enum class MusicAudioFormat { kUnknown, kMp3 };
MusicAudioFormat DetectMusicAudioFormat(const uint8_t*, size_t, const std::string&, const std::string&) {
    return MusicAudioFormat::kMp3;
}
const char* MusicAudioFormatName(MusicAudioFormat) { return "mp3"; }
int ToDecoderType(MusicAudioFormat) { return 1; }
void RegisterDecoders() {}
using esp_audio_simple_dec_handle_t = void*;
using esp_audio_err_t = int;
constexpr int ESP_AUDIO_ERR_OK = 0, ESP_AUDIO_ERR_BUFF_NOT_ENOUGH = 1;
constexpr int ESP_AUDIO_ERR_DATA_LACK = 2, ESP_AUDIO_ERR_CONTINUE = 3;
constexpr int ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE = 0;
struct esp_audio_simple_dec_cfg_t { int dec_type; void* dec_cfg; size_t cfg_size; bool use_frame_dec; };
struct esp_audio_simple_dec_raw_t { uint8_t* buffer; uint32_t len; bool eos; uint32_t consumed; int frame_recover; };
struct esp_audio_simple_dec_out_t { uint8_t* buffer; uint32_t len; uint32_t needed_size; uint32_t decoded_size; };
struct esp_audio_simple_dec_info_t { uint32_t sample_rate; uint8_t bits_per_sample; uint8_t channel; uint32_t bitrate; uint32_t frame_size; };
int esp_audio_simple_dec_open(esp_audio_simple_dec_cfg_t*, void** decoder) { *decoder = active; return 0; }
int esp_audio_simple_dec_close(void*) { return 0; }
int esp_audio_simple_dec_get_info(void*, esp_audio_simple_dec_info_t* info) {
    info->sample_rate = 1000; info->bits_per_sample = 16; info->channel = 1; return 0;
}
int esp_audio_simple_dec_process(void*, esp_audio_simple_dec_raw_t* raw, esp_audio_simple_dec_out_t* frame) {
    ++process_calls;
    if (!raw->len) return 0;
    if (phase == 1 && recovery_read) {
        if (partial_parser) assert(parser_waited && raw->buffer[0] == 0x11 && raw->buffer[1] == 0x22);
        phase = 2;
        active->net_done = true;
    }
    if (phase == 0 && active->storage.bytes.empty() && (mode >= 2 && mode != 3 && mode != 4)) {
        if (partial_parser && raw->len > 2) {
            raw->buffer[raw->len - 2] = 0x11;
            raw->buffer[raw->len - 1] = 0x22;
            raw->consumed = raw->len - 2;
        } else {
            if (partial_parser) parser_waited = true;
            phase = 1;
            if (partial_parser) return ESP_AUDIO_ERR_DATA_LACK;
            raw->consumed = raw->len;
        }
    } else {
        raw->consumed = raw->len;
    }
    std::fill_n(reinterpret_cast<int16_t*>(frame->buffer), 60, static_cast<int16_t>(output_frames + 1));
    frame->decoded_size = 120;
    if (mode == 0 || mode == 1 || mode == 3 || mode == 4) active->net_done = true;
    return 0;
}
void DownmixToMono16(const uint8_t* input, size_t bytes, int, int, std::vector<int16_t>& mono) {
    const auto* samples = reinterpret_cast<const int16_t*>(input);
    mono.assign(samples, samples + bytes / 2);
}
using esp_ae_rate_cvt_handle_t = void*;
using esp_ae_sample_t = int16_t*;
constexpr int ESP_AUDIO_BIT16 = 16, ESP_AE_RATE_CVT_PERF_TYPE_SPEED = 0, ESP_AE_ERR_OK = 0;
struct esp_ae_rate_cvt_cfg_t { uint32_t src_rate; uint32_t dest_rate; int channel; int bits_per_sample; int complexity; int perf_type; };
void esp_ae_rate_cvt_close(void*) {}
int esp_ae_rate_cvt_open(esp_ae_rate_cvt_cfg_t*, void**) { assert(false); return 1; }
void esp_ae_rate_cvt_get_max_out_sample_num(void*, size_t, uint32_t*) { assert(false); }
void esp_ae_rate_cvt_process(void*, int16_t*, size_t, int16_t*, uint32_t*) { assert(false); }
class MusicPlayer {
public:
    using Session = ::Session;
    bool WaitForBuffer(Session&, bool);
    void DecodeTask(const std::shared_ptr<Session>&);
    bool PushFrame(Session&, std::vector<int16_t>& pcm, uint32_t position) {
        assert(!pcm.empty() && pcm.front() == output_frames + 1);
        positions.push_back(position); ++output_frames; return true;
    }
    void ReportFailure(const std::shared_ptr<Session>&, const std::string& reason) { failure = reason; }
    void CheckFinished(const std::shared_ptr<Session>&) {}
};
'''
        driver += constants + "\n" + helper + decode
        driver += r'''
int main(int argc, char** argv) {
    assert(argc == 2);
    mode = std::stoi(argv[1]);
    auto session = std::make_shared<Session>();
    active = session.get();
    if (mode == 1) { session->local_stream = true; session->track.stream_url = "/sdcard/song.mp3"; }
    if (mode == 3) session->buffer_capacity = 32767;
    if (mode == 4) { Feed(2000); session->net_done = true; }
    if (mode == 11) session->live_stream = session->track.live = true;
    partial_parser = mode == 12;
    MusicPlayer player;
    player.DecodeTask(session);
    if (mode == 5) { assert(!first_read && !output_frames && session->cancelled); return 0; }
    if (mode == 6) { assert(!first_read && !output_frames && !failure.empty()); return 0; }
    if (mode == 0 || mode == 2 || mode >= 7) {
        if (mode != 11) assert(first_read >= 49152);
    }
    if (mode == 1 || mode == 11) assert(first_read == 16384);
    if (mode == 3) assert(first_read >= 28672 && first_read < 32767);
    if (mode == 4) assert(first_read == 2000 && session->decode_done);
    if (mode == 2 || mode == 7 || mode == 12) {
        assert(recovery_read >= 32768 && phase == 2 && session->decode_done);
        if (mode == 7) assert(paused_once);
        if (mode == 12) assert(parser_waited);
    }
    if (mode == 8) assert(cancelled_while_paused && session->cancelled && !recovery_read);
    if (mode == 9) assert(session->decode_done && !recovery_read);
    if (mode == 10) assert(!failure.empty() && !recovery_read);
    if (mode == 11) assert(recovery_read == 16384 && session->decode_done);
    for (size_t i = 0; i < positions.size(); ++i) assert(positions[i] == i * 60);
    assert(!positions.empty());
}
'''
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cpp = pathlib.Path(cls.temp.name) / "buffering.cc"
        cls.exe = pathlib.Path(cls.temp.name) / "buffering"
        cpp.write_text(driver)
        result = subprocess.run(
            [os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
             str(cpp), "-o", str(cls.exe)], capture_output=True, text=True,
        )
        if result.returncode:
            raise AssertionError(result.stderr)

    def run_mode(self, mode):
        result = subprocess.run([str(self.exe), str(mode)], capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_online_start_waits_for_a_reserve_and_small_capacity_remains_reachable(self):
        for mode in (0, 3):
            with self.subTest(mode=mode):
                self.run_mode(mode)

    def test_local_live_and_short_files_do_not_wait_for_online_reserve(self):
        for mode in (1, 4, 11):
            with self.subTest(mode=mode):
                self.run_mode(mode)

    def test_underrun_accumulates_bursts_and_preserves_partial_parser_and_pcm_order(self):
        for mode in (2, 12):
            with self.subTest(mode=mode):
                self.run_mode(mode)

    def test_waits_respond_to_pause_cancel_eof_and_failure(self):
        for mode in (5, 6, 7, 8, 9, 10):
            with self.subTest(mode=mode):
                self.run_mode(mode)


if __name__ == "__main__":
    unittest.main()
