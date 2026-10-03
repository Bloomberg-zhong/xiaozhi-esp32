#include "custom_lcd_display.h"

#include <driver/usb_serial_jtag.h>
#include <esp_log.h>

bool CustomLcdDisplay::ShouldShowUsbPowerIcon() const {
    // ETA6098 STAT only drives LED1 on this board; there is no MCU status pin.
    // SOF proves a powered USB host connection, even without an open terminal.
    // A wall adapter provides no SOF, so its charge current remains unknown.
    const bool connected = usb_serial_jtag_is_connected();
    static std::atomic<int> previous{-1};
    if (previous.exchange(connected) != connected)
        ESP_LOGI("RlcdPower", "USB host %s; icon=%s (charge current unavailable)",
                 connected ? "connected" : "disconnected", connected ? "bolt" : "battery");
    return connected;
}
