#include <cassert>
#include <cstdint>
#include <limits>

#include "music_ui_model.h"

int main() {
    using namespace rlcd_music_ui;
    assert(ShouldShowMusicPage(kDeviceStatePlaying, true, false));
    assert(ShouldShowMusicPage(kDeviceStateIdle, true, true));
    assert(!ShouldShowMusicPage(kDeviceStateIdle, true, false));
    assert(!ShouldShowMusicPage(kDeviceStatePlaying, false, false));
    for (auto state : {kDeviceStateConnecting, kDeviceStateListening, kDeviceStateSpeaking,
                       kDeviceStateNotifying, kDeviceStateStarting, kDeviceStateUpgrading,
                       kDeviceStateWifiConfiguring, kDeviceStateFatalError}) {
        assert(!ShouldShowMusicPage(state, true, true));
        assert(!ShouldShowMusicPage(state, true, false));
    }
    assert(MusicProgressPermille(0, 0) == 0);
    assert(MusicProgressPermille(1000, 0) == 0);
    assert(MusicProgressPermille(30000, 120000) == 250);
    assert(MusicProgressPermille(130000, 120000) == 1000);
    auto max = std::numeric_limits<uint32_t>::max();
    assert(MusicProgressPermille(max / 2, max) == 499);
    assert(MusicProgressPermille(max, max) == 1000);
    assert(FormatMusicTimeline(0, 0, false) == "00:00 / --:--");
    assert(FormatMusicTimeline(65000, 240000, false) == "01:05 / 04:00");
    assert(FormatMusicTimeline(65000, 0, true) == "01:05 / LIVE");
    assert(FormatMusicTimeline(0, 3600000, false) == "00:00 / 60:00");
}
