#pragma once

#include "sdkconfig.h"
#if CONFIG_USE_MUSIC_PLAYER
#include <functional>
#include <memory>
#include <string>

#include "lvgl_image.h"
#include "music_source.h"

// One latest-request worker; decoding and SD/network I/O never hold LVGL.
class MusicCoverLoader {
public:
    using Callback = std::function<void(uint32_t, std::shared_ptr<LvglAllocatedImage>)>;
    explicit MusicCoverLoader(Callback callback);
    ~MusicCoverLoader();
    void Request(uint32_t session, MusicTrack track, std::shared_ptr<MusicSource> source,
                 std::string root);

private:
    struct State;
    std::shared_ptr<State> state_;
    static void Run(void* argument);
};
#endif
