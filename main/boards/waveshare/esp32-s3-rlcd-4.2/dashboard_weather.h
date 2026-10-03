#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstdint>
#include <mutex>
#include <string>

class DashboardWeather {
public:
    static DashboardWeather& Instance();
    bool Start();
    bool Configure(const std::string& city);
    std::string GetCity() const;
    bool IsAutomatic() const;

private:
    DashboardWeather();
    static void TaskEntry(void* context);
    bool Fetch(const std::string& url, std::string& body);
    mutable std::mutex mutex_;
    std::string city_;
    bool automatic_ = true;
    uint32_t revision_ = 0;
    TaskHandle_t task_ = nullptr;
};
