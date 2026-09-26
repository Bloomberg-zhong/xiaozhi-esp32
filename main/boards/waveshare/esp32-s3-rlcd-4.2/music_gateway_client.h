#pragma once

#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "music_gateway_model.h"

namespace rlcd_dashboard {

struct MusicGatewayPlayback {
    MusicGatewaySong song;
    std::string audio_url;
    std::string lyric_url;
    bool used_fallback = false;
};

class MusicGatewayClient {
public:
    static MusicGatewayClient& Instance();

    bool Configure(const std::string& base_url, std::string& error);
    std::string GetBaseUrl() const;
    std::vector<MusicGatewaySong> Search(const std::string& query, const std::string& source,
                                         std::string& error);
    void SetSearchResults(const std::vector<MusicGatewaySong>& songs);
    std::optional<MusicGatewayPlayback> ResolvePlayback(size_t one_based_index, std::string& error);
    std::optional<MusicGatewayPlayback> ResolveRelativePlayback(int offset, std::string& error);

private:
    MusicGatewayClient();

    bool GetJson(const std::string& url, std::string& body, std::string& error) const;
    bool InspectPlayable(const std::string& base_url, const MusicGatewaySong& song, bool& playable,
                         std::string& error) const;
    std::optional<MusicGatewaySong> SwitchSource(const std::string& base_url,
                                                 const MusicGatewaySong& song,
                                                 std::string& error) const;

    mutable std::mutex mutex_;
    std::string base_url_;
    std::vector<MusicGatewaySong> search_results_;
    size_t current_search_index_ = 0;
    bool has_current_search_index_ = false;
};

}  // namespace rlcd_dashboard
