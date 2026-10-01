#ifndef RLCD_MUSIC_UI_MODEL_H_
#define RLCD_MUSIC_UI_MODEL_H_

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

#include "device_state.h"

namespace rlcd_music_ui {

inline bool ShouldShowMusicPage(DeviceState state, bool has_session, bool paused) {
    return has_session && (state == kDeviceStatePlaying || (state == kDeviceStateIdle && paused));
}

inline int MusicProgressPermille(uint32_t position_ms, uint32_t duration_ms) {
    if (duration_ms == 0) {
        return 0;
    }
    return static_cast<int>(static_cast<uint64_t>(std::min(position_ms, duration_ms)) * 1000 /
                            duration_ms);
}

inline std::string FormatMusicTime(uint32_t milliseconds) {
    uint32_t seconds = milliseconds / 1000;
    char text[24];
    std::snprintf(text, sizeof(text), "%02lu:%02lu", static_cast<unsigned long>(seconds / 60),
                  static_cast<unsigned long>(seconds % 60));
    return text;
}

inline std::string FormatMusicTimeline(uint32_t position_ms, uint32_t duration_ms, bool live) {
    return FormatMusicTime(position_ms) + " / " +
           (live               ? "LIVE"
            : duration_ms == 0 ? "--:--"
                               : FormatMusicTime(duration_ms));
}

}  // namespace rlcd_music_ui

#endif  // RLCD_MUSIC_UI_MODEL_H_
