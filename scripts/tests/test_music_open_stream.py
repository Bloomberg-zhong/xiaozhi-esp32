"""Exercise production stream opening with controlled HTTP timing and permissions."""
import os
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


class MusicOpenStreamTests(unittest.TestCase):
    def test_range_resume_stops_promptly_when_background_permission_is_revoked(self):
        self.run_case("revoked")

    def test_ignored_range_has_poll_timeout_and_wrap_safe_total_deadline(self):
        self.run_case("deadline")

    def test_default_foreground_range_and_exact_skip_preserve_resume_position(self):
        self.run_case("range")

    def test_background_permission_gates_requests_and_redirects(self):
        self.run_case("redirect")

    def run_case(self, case):
        source = (ROOT / "main/music/music_player.cc").read_text()
        start = source.rfind("\n", 0, source.index("MusicPlayer::OpenStream(")) + 1
        method = source[start:source.index("size_t MusicPlayer::WriteToBuffer(")]
        signature = method.split("{", 1)[0]
        # Only call arity adapts during the RED stage; the production body is
        # executed unchanged before and after the optional permission API exists.
        has_permission = "std::function" in signature
        declaration = signature.replace("MusicPlayer::", "").strip()
        driver = r'''
#include "music_source.h"
#include "music_util.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>
#define TAG "test"
void Log(const char*, const char*, ...) {}
#define ESP_LOGI(...) Log(__VA_ARGS__)
#define ESP_LOGW(...) Log(__VA_ARGS__)
#define ESP_LOGE(...) Log(__VA_ARGS__)
#define pdMS_TO_TICKS(x) (x)
using TickType_t = uint32_t;
static TickType_t ticks = 0;
TickType_t xTaskGetTickCount() { return ticks; }
constexpr int kMaxRedirects = 3, kStreamTimeoutMs = 10000;
[[maybe_unused]] constexpr int kReadPollMs = 1000;
enum class NetworkErrc { Timeout };
struct Error {
    NetworkErrc code = NetworkErrc::Timeout;
    std::string ToString() const { return "controlled timeout"; }
};
template<class T> struct Result : std::optional<T> {
    using std::optional<T>::optional;
    Error error() const { return {}; }
};
struct Fixture {
    int status = 200, created = 0, closed = 0, reads = 0, timeout = 0;
    int connection_id = -1;
    size_t body_length = 4096, delivered = 0, read_step = 512;
    TickType_t elapsed_per_read = 0;
    bool permitted = true, revoke_after_read = false, revoke_on_open = false;
    std::string range, location = "https://source/redirected";
    std::map<std::string, std::string> request;
    std::vector<size_t> capacities;
};
static Fixture fixture;
class Http {
public:
    void SetTimeout(int timeout) { fixture.timeout = timeout; }
    void SetHeader(const std::string& key, const std::string& value) {
        fixture.request[key] = value;
    }
    Result<bool> Open(const std::string& method, const std::string& url) {
        assert(method == "GET" && url.rfind("https://source/", 0) == 0);
        if (fixture.revoke_on_open) fixture.permitted = false;
        return true;
    }
    void Close() { ++fixture.closed; }
    Result<int> GetStatusCode() { return fixture.status; }
    size_t GetBodyLength() { return fixture.body_length; }
    std::string GetResponseHeader(const std::string& key) const {
        if (key == "Content-Range") return fixture.range;
        if (key == "Location") return fixture.location;
        return "audio/mpeg";
    }
    Result<int> Read(char* out, size_t capacity) {
        fixture.capacities.push_back(capacity);
        ++fixture.reads;
        ticks += fixture.elapsed_per_read;
        const size_t size = std::min(capacity, fixture.read_step);
        std::memset(out, 'a', size);
        fixture.delivered += size;
        if (fixture.revoke_after_read) fixture.permitted = false;
        return static_cast<int>(size);
    }
};
class Network {
public:
    std::unique_ptr<Http> CreateHttp(int id) {
        ++fixture.created;
        fixture.connection_id = id;
        return std::make_unique<Http>();
    }
};
class Board {
    Network network_;
public:
    static Board& GetInstance() { static Board board; return board; }
    Network* GetNetwork() { return &network_; }
};
struct Session {
    MusicTrack track;
    std::shared_ptr<MusicSource> source;
    std::atomic<bool> cancelled{false};
    std::mutex mutex;
    std::string content_type;
};
class MusicPlayer {
public:
    using Session = ::Session;
'''
        driver += declaration + ";\n};\n" + method
        driver += r'''
std::unique_ptr<Http> OpenPermitted(MusicPlayer& player, Session& session,
                                    size_t offset, size_t& total) {
'''
        driver += ("return player.OpenStream(session, offset, total, 8, "
                   "[] { return fixture.permitted; });\n" if has_permission else
                   "return player.OpenStream(session, offset, total, 8);\n")
        driver += "}\n"
        default_call = ("player.OpenStream(session, 5, total, 4, {})" if has_permission else
                        "player.OpenStream(session, 5, total, 4)")
        ignored_call = ("player.OpenStream(session, 1000, total, 4, {})" if has_permission else
                        "player.OpenStream(session, 1000, total, 4)")
        driver += r'''
int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string test = argv[1];
    MusicPlayer player;
    Session session;
    session.track.stream_url = "https://source/audio";
    size_t total = 0;
    if (test == "revoked") {
        fixture.revoke_after_read = true;
        auto http = OpenPermitted(player, session, 2048, total);
        assert(!http && fixture.closed == 1);
        assert(fixture.reads == 1 && fixture.delivered == 512);
        assert(fixture.timeout <= 1000);
    } else if (test == "deadline") {
        ticks = UINT32_MAX - 4095;  // Deadline must survive the RTOS tick wrap.
        const TickType_t start = ticks;
        fixture.read_step = 1;
        fixture.elapsed_per_read = 1000;
        auto http = OpenPermitted(player, session, 100000, total);
        assert(!http && fixture.closed == 1);
        assert(fixture.reads <= 10 && TickType_t(ticks - start) <= 10000);
        assert(fixture.timeout <= 1000);
    } else if (test == "range") {
        fixture.status = 206;
        fixture.body_length = 5;
        fixture.range = "bytes 5-9/10";
        auto http = DEFAULT_CALL;
        assert(http && total == 10 && fixture.reads == 0);
        assert(fixture.request.at("Range") == "bytes=5-");
        assert(fixture.request.at("Accept-Encoding") == "identity");
        assert(fixture.connection_id == 4);
        fixture = Fixture{};
        auto ignored = IGNORED_CALL;
        assert(ignored && fixture.delivered == 1000);
        assert(fixture.capacities.size() == 2 && fixture.capacities[0] == 512 &&
               fixture.capacities[1] == 488);
        fixture = Fixture{};
        fixture.status = 206;
        fixture.body_length = 6;  // Invalid Range length cannot become resume bytes.
        fixture.range = "bytes 5-9/10";
        auto invalid = DEFAULT_CALL;
        assert(!invalid && fixture.closed == 1 && fixture.reads == 0);
    } else if (test == "redirect") {
        fixture.permitted = false;
        assert(!OpenPermitted(player, session, 1024, total));
        assert(fixture.created == 0);
        fixture = Fixture{};
        fixture.status = 302;
        fixture.revoke_on_open = true;
        assert(!OpenPermitted(player, session, 1024, total));
        assert(fixture.created == 1 && fixture.closed == 1 && fixture.reads == 0);
    } else {
        assert(false);
    }
}
'''.replace("DEFAULT_CALL", default_call).replace("IGNORED_CALL", ignored_call)
        with tempfile.TemporaryDirectory() as temp:
            cpp = pathlib.Path(temp) / "open-stream.cc"
            cpp.write_text(driver)
            exe = pathlib.Path(temp) / "open-stream"
            result = subprocess.run([
                os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-I" + str(ROOT / "main/music"), str(cpp),
                str(ROOT / "main/music/music_util.cc"), "-o", str(exe)],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe), case], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
