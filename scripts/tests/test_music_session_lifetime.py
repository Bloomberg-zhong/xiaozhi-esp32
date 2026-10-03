"""Exercise the production Session allocation/destructor with host ESP API doubles.

The faulty IDF 6.0.2 DeleteWithCaps path is represented at the API boundary:
it deletes a stream as a semaphore, then frees the same control block again.
The test executes the actual Session definition and allocation code, and checks
that retiring sessions releases each allocation once, including failure paths.
"""

import os
import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]


class MusicSessionLifetimeTest(unittest.TestCase):
    def test_retired_sessions_release_buffers_once(self):
        source = (ROOT / "main/music/music_player.cc").read_text()
        session = source.split("struct MusicPlayer::Session {", 1)[1].split(
            "struct MusicPlayer::TaskContext", 1
        )[0]
        allocation = source.split("bool MusicPlayer::StartTasks(", 1)[1].split(
            "auto* net_context", 1
        )[0].split("{", 1)[1]
        driver = r'''
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include "music_source.h"
#include "music_cache.h"
using UBaseType_t = unsigned;
using BaseType_t = int;
constexpr unsigned MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2, MALLOC_CAP_INTERNAL = 4;
constexpr size_t kStreamBufferSize = 256 * 1024, kFallbackBufferSize = 32 * 1024;
bool IsLocalMusicPath(const std::string& path) { return !path.empty() && path.front() == '/'; }
#define ESP_LOGE(...) ((void)0)
#define TAG "test"
struct StaticStreamBuffer_t { unsigned char* storage; bool active; };
using StreamBufferHandle_t = StaticStreamBuffer_t*;
static std::unordered_set<void*> allocations;
static int allocation_calls = 0, fail_allocation = 0;
static bool fail_create = false;
void* heap_caps_malloc(size_t size, unsigned) {
    if (++allocation_calls == fail_allocation || fail_allocation == -1) return nullptr;
    void* p = std::malloc(size);
    assert(p);
    allocations.insert(p);
    return p;
}
void heap_caps_free(void* p) {
    if (!p) return;
    if (allocations.erase(p) != 1) {
        std::fprintf(stderr, "session double free or invalid free\n");
        std::exit(66);
    }
    std::free(p);
}
StreamBufferHandle_t xStreamBufferCreateStatic(size_t size, size_t trigger,
                                               unsigned char* storage, StaticStreamBuffer_t* control) {
    assert(size > 0 && trigger == 1 && allocations.count(storage));
    if (fail_create) return nullptr;
    control->storage = storage;
    control->active = true;
    return control;
}
void vStreamBufferDelete(StreamBufferHandle_t control) {
    assert(control && control->active);
    control->active = false;
}
StreamBufferHandle_t xStreamBufferCreateWithCaps(size_t size, size_t trigger, unsigned caps) {
    auto* control = static_cast<StaticStreamBuffer_t*>(heap_caps_malloc(sizeof(StaticStreamBuffer_t), caps));
    auto* storage = static_cast<unsigned char*>(heap_caps_malloc(size, caps));
    if (!control || !storage) { heap_caps_free(control); heap_caps_free(storage); return nullptr; }
    return xStreamBufferCreateStatic(size, trigger, storage, control);
}
void vStreamBufferDeleteWithCaps(StreamBufferHandle_t control) {
    auto* storage = control->storage;
    // IDF 6.0.2 uses vSemaphoreDelete here; a stream is not a Queue_t.
    heap_caps_free(control);
    heap_caps_free(control);
    heap_caps_free(storage);
}
'''
        driver += "struct Session {" + session
        driver += "bool Allocate(const std::shared_ptr<Session>& session) {" + allocation + "return true; }\n"
        driver += r'''
int main() {
    for (int i = 0; i < 100; ++i) {
        auto session = std::make_shared<Session>();
        assert(Allocate(session));
        auto reader = session, decoder = session;
        session->cancelled = true;
        session.reset();
        reader.reset();
        assert(!allocations.empty()); // decoder still owns its buffer
        decoder.reset();
        assert(allocations.empty());
    }
    fail_allocation = allocation_calls + 1; // PSRAM fails, internal fallback works
    { auto session = std::make_shared<Session>(); assert(Allocate(session)); }
    assert(allocations.empty());
    fail_allocation = -1;
    { auto session = std::make_shared<Session>(); assert(!Allocate(session)); }
    assert(allocations.empty());
    fail_allocation = 0;
    fail_create = true;
    { auto session = std::make_shared<Session>(); assert(!Allocate(session)); }
    assert(allocations.empty());
}
'''
        with tempfile.TemporaryDirectory() as temp:
            cpp = pathlib.Path(temp) / "session.cc"
            exe = pathlib.Path(temp) / "session"
            cpp.write_text(driver)
            compile_result = subprocess.run(
                [os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 "-I" + str(ROOT / "main/music"), str(cpp), str(ROOT / "main/music/music_cache.cc"),
                 str(ROOT / "main/music/music_util.cc"), "-o", str(exe)],
                capture_output=True, text=True,
            )
            self.assertEqual(compile_result.returncode, 0, compile_result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
