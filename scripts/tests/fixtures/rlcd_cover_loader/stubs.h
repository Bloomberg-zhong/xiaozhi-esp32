#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "music_source.h"

constexpr int ESP_OK = 0;
constexpr int ESP_FAIL = -1;
constexpr int MALLOC_CAP_SPIRAM = 1;
constexpr int MALLOC_CAP_8BIT = 2;
constexpr int LV_COLOR_FORMAT_RGB565 = 1;
constexpr int pdPASS = 1;
constexpr int pdTRUE = 1;
inline unsigned pdMS_TO_TICKS(unsigned value) { return value; }
inline void* heap_caps_malloc(size_t size, int) { return std::malloc(size); }
inline void heap_caps_free(void* data) { std::free(data); }
inline std::atomic<int64_t> fake_time{0};
inline int64_t esp_timer_get_time() { return fake_time.load(); }

struct FakeTask {
    std::mutex mutex;
    std::condition_variable wake;
    unsigned notifications = 0;
    std::thread thread;
};
using TaskHandle_t = FakeTask*;
inline thread_local FakeTask* current_task = nullptr;
inline std::vector<std::unique_ptr<FakeTask>> tasks;
inline std::atomic<int> workers{0}, peak_workers{0}, created_workers{0};
inline int xTaskCreate(void (*run)(void*), const char*, unsigned, void* argument, int,
                       TaskHandle_t* handle) {
    auto task = std::make_unique<FakeTask>();
    *handle = task.get();
    auto* pointer = task.get();
    ++created_workers;
    task->thread = std::thread([=] {
        current_task = pointer;
        const int count = ++workers;
        peak_workers.store(std::max(peak_workers.load(), count));
        run(argument);
        --workers;
    });
    tasks.push_back(std::move(task));
    return pdPASS;
}
inline void xTaskNotifyGive(TaskHandle_t task) {
    std::lock_guard<std::mutex> lock(task->mutex);
    ++task->notifications;
    task->wake.notify_all();
}
inline unsigned ulTaskNotifyTake(int, unsigned) {
    std::unique_lock<std::mutex> lock(current_task->mutex);
    current_task->wake.wait_for(lock, std::chrono::milliseconds(1),
                                [] { return current_task->notifications != 0; });
    const unsigned count = current_task->notifications;
    current_task->notifications = 0;
    return count;
}
inline void vTaskDelete(void*) {}

enum DeviceState {
    kDeviceStateIdle,
    kDeviceStatePlaying,
    kDeviceStateListening,
    kDeviceStateSpeaking
};
class Application {
public:
    std::atomic<DeviceState> mode{kDeviceStatePlaying};
    std::mutex mutex;
    std::vector<std::function<void()>> scheduled;
    static Application& GetInstance() {
        static Application app;
        return app;
    }
    DeviceState GetDeviceState() const { return mode.load(); }
    void Schedule(std::function<void()>&& callback) {
        std::lock_guard<std::mutex> lock(mutex);
        scheduled.push_back(std::move(callback));
    }
    size_t ScheduledCount() {
        std::lock_guard<std::mutex> lock(mutex);
        return scheduled.size();
    }
    void Drain() {
        std::vector<std::function<void()>> callbacks;
        {
            std::lock_guard<std::mutex> lock(mutex);
            callbacks.swap(scheduled);
        }
        for (auto& callback : callbacks)
            callback();
    }
};

struct FakeResponse {
    std::string body;
    int status = 200;
    std::string location;
};
struct FakeNetworkState {
    std::mutex mutex;
    std::condition_variable wake;
    std::map<std::string, FakeResponse> responses;
    std::vector<std::string> opened;
    bool block_reads = false;
    int active_requests = 0, reads = 0;
    std::atomic<bool> interrupt_on_read{false};
    std::atomic<int64_t> advance_per_read{0};
};
inline FakeNetworkState network_state;
class Http {
public:
    void SetTimeout(int) {}
    void SetHeader(const std::string&, const std::string&) {}
    bool Open(const char*, const std::string& url) {
        std::lock_guard<std::mutex> lock(network_state.mutex);
        network_state.opened.push_back(url);
        ++network_state.active_requests;
        active_ = true;
        auto found = network_state.responses.find(url);
        response_ =
            found == network_state.responses.end() ? FakeResponse{"", 404, ""} : found->second;
        network_state.wake.notify_all();
        return true;
    }
    std::optional<int> GetStatusCode() { return response_.status; }
    std::string GetResponseHeader(const char*) { return response_.location; }
    std::optional<int> Read(char* data, size_t size) {
        {
            std::unique_lock<std::mutex> lock(network_state.mutex);
            network_state.wake.wait(lock, [] { return !network_state.block_reads; });
            ++network_state.reads;
        }
        fake_time += network_state.advance_per_read.load();
        if (network_state.interrupt_on_read.exchange(false))
            Application::GetInstance().mode = kDeviceStateListening;
        const size_t count = std::min(size, response_.body.size() - position_);
        std::memcpy(data, response_.body.data() + position_, count);
        position_ += count;
        return static_cast<int>(count);
    }
    void Close() {
        std::lock_guard<std::mutex> lock(network_state.mutex);
        if (active_) {
            --network_state.active_requests;
            active_ = false;
        }
        network_state.wake.notify_all();
    }
    ~Http() { Close(); }

private:
    bool active_ = false;
    size_t position_ = 0;
    FakeResponse response_;
};
class FakeNetwork {
public:
    std::unique_ptr<Http> CreateHttp(int) { return std::make_unique<Http>(); }
};
class Board {
public:
    static Board& GetInstance() {
        static Board board;
        return board;
    }
    FakeNetwork* GetNetwork() { return &network_; }

private:
    FakeNetwork network_;
};

inline std::atomic<int> live_images{0}, decoder_calls{0};
class LvglAllocatedImage {
public:
    LvglAllocatedImage(void* data, size_t size, int width, int height, int stride, int)
        : data_(data) {
        if (size != 128 * 128 * 2 || width != 128 || height != 128 || stride != 256)
            std::abort();
        ++live_images;
    }
    ~LvglAllocatedImage() {
        std::free(data_);
        --live_images;
    }

private:
    void* data_;
};
inline int jpeg_to_image(const uint8_t* bytes, size_t length, uint8_t** data, size_t* size,
                         size_t* width, size_t* height, size_t* stride) {
    ++decoder_calls;
    // Exercise production header validation separately; this stub simulates
    // decoder rejection after a valid SOF, and success with bounded RGB565.
    if (!length || bytes[length - 1] != 1)
        return ESP_FAIL;
    *width = *height = 8;
    *stride = 16;
    *size = 128;
    *data = static_cast<uint8_t*>(std::malloc(*size));
    std::memset(*data, 0xff, *size);
    return ESP_OK;
}
