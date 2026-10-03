#pragma once
#include <driver/i2c_master.h>

// Uses the board's existing GPIO13/14 bus. Acquisition runs away from the
// application/audio/LVGL tasks; optional peripherals never abort boot.
class DashboardSensors {
public:
    explicit DashboardSensors(i2c_master_bus_handle_t bus);
    bool Start();

private:
    i2c_master_dev_handle_t sensor_ = nullptr;
    i2c_master_dev_handle_t rtc_ = nullptr;
    bool Command(uint16_t command);
    void ReadEnvironment();
    void SyncRtc();
    static void TaskEntry(void* context);
};
