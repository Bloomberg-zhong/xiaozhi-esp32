#include "dashboard_sensors.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sys/time.h>
#include <array>
#include <ctime>
#include "dashboard_store.h"

namespace {
constexpr const char* TAG = "DesktopSensors";
uint8_t ToBcd(int value) { return ((value / 10) << 4) | (value % 10); }
int FromBcd(uint8_t value) {
    return (value & 15) <= 9 && (value >> 4) <= 9 ? (value >> 4) * 10 + (value & 15) : -1;
}
bool ValidTime(const std::tm& value) {
    return value.tm_year >= 125 && value.tm_year <= 199 && value.tm_mon >= 0 && value.tm_mon < 12 &&
           value.tm_mday >= 1 &&
           value.tm_mday <= rlcd_dashboard::DaysInMonth(value.tm_year + 1900, value.tm_mon + 1) &&
           value.tm_hour >= 0 && value.tm_hour < 24 && value.tm_min >= 0 && value.tm_min < 60 &&
           value.tm_sec >= 0 && value.tm_sec < 60;
}
}  // namespace

DashboardSensors::DashboardSensors(i2c_master_bus_handle_t bus) {
    for (auto address : {0x70, 0x51}) {
        if (i2c_master_probe(bus, address, 20) != ESP_OK) {
            ESP_LOGW(TAG, "Optional I2C device 0x%02x unavailable", address);
            continue;
        }
        i2c_device_config_t config{};
        config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        config.device_address = address;
        config.scl_speed_hz = 100000;
        auto* device = address == 0x70 ? &sensor_ : &rtc_;
        if (i2c_master_bus_add_device(bus, &config, device) != ESP_OK) {
            *device = nullptr;
        }
    }
    SyncRtc();  // Offline boot may recover the calendar from the backed-up RTC.
}

bool DashboardSensors::Start() {
    if (!sensor_ && !rtc_)
        return false;
    return xTaskCreate(TaskEntry, "desktop_sensors", 4096, this, 1, nullptr) == pdPASS;
}

bool DashboardSensors::Command(uint16_t command) {
    uint8_t bytes[] = {static_cast<uint8_t>(command >> 8), static_cast<uint8_t>(command)};
    if (!sensor_)
        return false;
    auto result = i2c_master_transmit(sensor_, bytes, sizeof(bytes), 30);
    if (result != ESP_OK)
        ESP_LOGW(TAG, "SHTC3 command 0x%04x: %s", command, esp_err_to_name(result));
    return result == ESP_OK;
}

void DashboardSensors::ReadEnvironment() {
    if (!sensor_)
        return;
    std::optional<rlcd_dashboard::RoomEnvironment> value;
    if (Command(0x3517)) {  // wake
        // Round up and add a tick: vTaskDelay(1) may return just before the
        // next tick, and pdMS_TO_TICKS(2) is zero at this board's 100 Hz.
        vTaskDelay(pdMS_TO_TICKS(2) + 1 + 1);
        if (Command(0x7866)) {                      // temperature first, no clock stretching
            vTaskDelay(pdMS_TO_TICKS(13) + 1 + 1);  // at least the 12.1 ms maximum conversion time
            uint8_t bytes[6]{};
            auto result = i2c_master_receive(sensor_, bytes, sizeof(bytes), 30);
            if (result == ESP_OK) {
                value = rlcd_dashboard::DecodeShtc3Sample(bytes, sizeof(bytes));
                if (!value)
                    ESP_LOGW(TAG, "SHTC3 CRC rejected measurement");
            } else {
                ESP_LOGW(TAG, "SHTC3 read: %s", esp_err_to_name(result));
            }
        }
    }
    Command(0xb098);  // sleep even when acquisition/CRC fails
    rlcd_dashboard::DashboardStore::Instance().UpdateRoomEnvironment(value);
    if (value)
        ESP_LOGI(TAG, "Indoor %.1f C, %.1f %%RH", value->temperature_c, value->humidity_percent);
    else
        ESP_LOGW(TAG, "SHTC3 measurement unavailable");
}

void DashboardSensors::SyncRtc() {
    if (!rtc_)
        return;
    time_t now = time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    if (ValidTime(local)) {
        // PCF85063 seconds start at 0x04. Write in one transaction so the
        // calendar and time cannot be torn across a second/minute rollover.
        uint8_t bytes[] = {0x04,
                           ToBcd(local.tm_sec),
                           ToBcd(local.tm_min),
                           ToBcd(local.tm_hour),
                           ToBcd(local.tm_mday),
                           ToBcd(local.tm_wday),
                           ToBcd(local.tm_mon + 1),
                           ToBcd(local.tm_year % 100)};
        if (i2c_master_transmit(rtc_, bytes, sizeof(bytes), 30) != ESP_OK) {
            ESP_LOGW(TAG, "Could not update RTC");
        }
        return;
    }
    uint8_t reg = 0x04, bytes[7]{};
    if (i2c_master_transmit_receive(rtc_, &reg, 1, bytes, sizeof(bytes), 30) != ESP_OK ||
        (bytes[0] & 0x80))
        return;  // oscillator-stop flag: time cannot be trusted
    local.tm_sec = FromBcd(bytes[0] & 0x7f);
    local.tm_min = FromBcd(bytes[1] & 0x7f);
    local.tm_hour = FromBcd(bytes[2] & 0x3f);
    local.tm_mday = FromBcd(bytes[3] & 0x3f);
    local.tm_mon = FromBcd(bytes[5] & 0x1f) - 1;
    local.tm_year = FromBcd(bytes[6]) + 100;
    local.tm_isdst = -1;
    if (!ValidTime(local))
        return;
    timeval tv{};
    tv.tv_sec = mktime(&local);
    if (tv.tv_sec > 0 && settimeofday(&tv, nullptr) == 0) {
        ESP_LOGI(TAG, "Recovered system time from PCF85063 RTC");
    }
}

void DashboardSensors::TaskEntry(void* context) {
    auto* sensors = static_cast<DashboardSensors*>(context);
    for (;;) {
        sensors->ReadEnvironment();
        sensors->SyncRtc();
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}
