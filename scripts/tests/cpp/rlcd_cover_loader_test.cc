#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "music_cache.h"
#include "music_cover_loader.h"
#include "stubs.h"

std::vector<LyricLine> MusicSource::FetchLyrics(const MusicTrack&) { return {}; }

class Source : public MusicSource {
public:
    const char* type() const override { return "http"; }
    const std::string& base_url() const override { return base_; }
    bool Search(const std::string&, const std::string&, int, std::vector<MusicTrack>&,
                std::string&) override {
        return false;
    }
    bool Ping(std::string&) override { return true; }
    MusicTrack BuildTrack(const std::string&, const std::string&, const std::string&,
                          bool) const override {
        return {};
    }

private:
    std::string base_ = "https://fixture.example";
};

bool Wait(const std::function<bool()>& condition) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!condition() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return condition();
}
void FinishWorkers() {
    assert(Wait([] { return workers == 0; }));
    for (auto& task : tasks)
        task->thread.join();
    tasks.clear();
}
void Reset() {
    assert(workers == 0);
    Application::GetInstance().Drain();
    assert(live_images == 0);
    Application::GetInstance().mode = kDeviceStatePlaying;
    std::lock_guard<std::mutex> lock(network_state.mutex);
    network_state.responses.clear();
    network_state.opened.clear();
    network_state.block_reads = false;
    network_state.active_requests = network_state.reads = 0;
    network_state.interrupt_on_read = false;
    network_state.advance_per_read = 0;
    fake_time = 0;
    created_workers = peak_workers = decoder_calls = 0;
}
std::string Jpeg(bool valid = true) {
    const uint8_t bytes[] = {
        0xff, 0xd8, 0xff, 0xc0, 0, 11,   8, 0,
        8,    0,    8,    1,    1, 0x11, 0, static_cast<uint8_t>(valid ? 1 : 0)};
    return {reinterpret_cast<const char*>(bytes), sizeof(bytes)};
}
MusicTrack Track(const std::string& id) {
    MusicTrack track;
    track.id = id;
    track.title = id;
    track.provider = "fixture";
    track.stream_url = "https://fixture.example/audio/" + id;
    track.cover_url = "https://fixture.example/cover/" + id;
    return track;
}
void Response(const MusicTrack& track, std::string body) {
    std::lock_guard<std::mutex> lock(network_state.mutex);
    network_state.responses[track.cover_url] = {std::move(body), 200, ""};
}
size_t Opens() {
    std::lock_guard<std::mutex> lock(network_state.mutex);
    return network_state.opened.size();
}
void ReleaseReads() {
    std::lock_guard<std::mutex> lock(network_state.mutex);
    network_state.block_reads = false;
    network_state.wake.notify_all();
}
bool IdleHttp() {
    std::lock_guard<std::mutex> lock(network_state.mutex);
    return network_state.active_requests == 0;
}

void InterruptedVoice(const std::string& root) {
    Reset();
    const auto track = Track("voice");
    Response(track, Jpeg());
    network_state.interrupt_on_read = true;
    std::vector<uint32_t> delivered;
    {
        MusicCoverLoader loader([&](uint32_t session, auto) { delivered.push_back(session); });
        loader.Request(7, track, std::make_shared<Source>(), root);
        assert(Wait([] {
            return Opens() == 1 && IdleHttp() &&
                   Application::GetInstance().mode == kDeviceStateListening;
        }));
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        assert(Application::GetInstance().ScheduledCount() == 0);
        Application::GetInstance().mode = kDeviceStatePlaying;
        assert(Wait([] { return Application::GetInstance().ScheduledCount() == 1; }));
        assert(Opens() == 2);
        Application::GetInstance().Drain();
        assert(delivered == std::vector<uint32_t>{7});
        assert(created_workers == 1 && peak_workers == 1);
    }
    FinishWorkers();
}

void LatestWins(const std::string& root) {
    Reset();
    auto first = Track("first");
    Response(first, Jpeg());
    network_state.block_reads = true;
    std::vector<uint32_t> delivered;
    {
        MusicCoverLoader loader([&](uint32_t session, auto) { delivered.push_back(session); });
        loader.Request(1, first, std::make_shared<Source>(), root);
        assert(Wait([] { return Opens() == 1; }));
        for (uint32_t session = 2; session <= 20; ++session) {
            auto track = Track("song" + std::to_string(session));
            Response(track, Jpeg());
            loader.Request(session, track, std::make_shared<Source>(), root);
        }
        ReleaseReads();
        assert(Wait([] { return Application::GetInstance().ScheduledCount() == 1; }));
        Application::GetInstance().Drain();
        assert(delivered == std::vector<uint32_t>{20});
        assert(Opens() == 2);
        assert(created_workers == 1 && peak_workers == 1);
    }
    FinishWorkers();
}

void CorruptCache(const std::string& root) {
    Reset();
    const auto track = Track("cached");
    auto source = std::make_shared<Source>();
    MusicCache cache(root);
    const auto invalid = Jpeg(false);
    assert(cache.StoreCover(track, source->base_url(), invalid.data(), invalid.size()));
    Response(track, Jpeg());
    std::vector<uint32_t> delivered;
    {
        MusicCoverLoader loader([&](uint32_t session, auto) { delivered.push_back(session); });
        loader.Request(1, track, source, root);
        assert(Wait([] { return Application::GetInstance().ScheduledCount() == 1; }));
        Application::GetInstance().Drain();
        assert(Opens() == 1 && decoder_calls == 2);
        std::ifstream file(cache.CoverPath(track, source->base_url()), std::ios::binary);
        assert(std::string(std::istreambuf_iterator<char>(file), {}) == Jpeg());
        loader.Request(2, track, source, root);
        assert(Wait([] { return Application::GetInstance().ScheduledCount() == 1; }));
        Application::GetInstance().Drain();
        assert(Opens() == 1);
        assert((delivered == std::vector<uint32_t>{1, 2}));
    }
    FinishWorkers();
}

MusicTrack CacheAudio(const std::string& root, const MusicTrack& online,
                      const std::shared_ptr<Source>& source) {
    MusicCache cache(root);
    auto writer = cache.Begin(online, source->base_url(), 8);
    assert(writer && writer->Append(0, "ID3saved", 8) && writer->Finish());
    auto entry = cache.Find(online, source->base_url());
    assert(entry);
    return entry->track;
}

void LocalMissingCover(const std::string& root) {
    Reset();
    auto source = std::make_shared<Source>();
    const auto online = Track("local-missing");
    const auto local = CacheAudio(root, online, source);
    const auto cover = local.stream_url.substr(0, local.stream_url.rfind('.')) + ".cover.jpg";
    Response(online, Jpeg());
    std::vector<uint32_t> delivered;
    {
        MusicCoverLoader loader([&](uint32_t session, auto) { delivered.push_back(session); });
        loader.Request(1, local, source, root);
        assert(Wait([] { return Application::GetInstance().ScheduledCount() == 1; }));
        Application::GetInstance().Drain();
        assert(Opens() == 1 && std::filesystem::exists(cover));
        assert(MusicCache(root).CoverPath(local, source->base_url()) == cover);
        assert(MusicCache(root).CoverPath(online, source->base_url()) == cover);
        MusicTrack again;
        assert(MusicCache::ReadTrack(local.stream_url, again));
        loader.Request(2, again, source, root);
        assert(Wait([] { return Application::GetInstance().ScheduledCount() == 1; }));
        Application::GetInstance().Drain();
        assert(Opens() == 1 && (delivered == std::vector<uint32_t>{1, 2}));
        size_t covers = 0;
        for (const auto& file : std::filesystem::directory_iterator(root + "/music-cache"))
            covers += file.path().extension() == ".jpg";
        assert(covers == 1);  // Local IDs must not create a second artwork identity.
    }
    FinishWorkers();
}

void LocalCorruptCover(const std::string& root) {
    Reset();
    auto source = std::make_shared<Source>();
    const auto online = Track("local-corrupt");
    CacheAudio(root, online, source);
    MusicCache cache(root);
    const auto bad = Jpeg(false);
    assert(cache.StoreCover(online, source->base_url(), bad.data(), bad.size()));
    auto entry = cache.Find(online, source->base_url());
    assert(entry && entry->track.cover_url.front() == '/');
    Response(online, Jpeg());
    {
        MusicCoverLoader loader([](uint32_t, auto) {});
        loader.Request(1, entry->track, source, root);
        assert(Wait([] { return Application::GetInstance().ScheduledCount() == 1; }));
        Application::GetInstance().Drain();
        std::ifstream file(entry->track.cover_url, std::ios::binary);
        assert(std::string(std::istreambuf_iterator<char>(file), {}) == Jpeg());
        assert(Opens() == 1 && decoder_calls == 2);
    }
    FinishWorkers();
}

void LocalUserCover(const std::string& root) {
    Reset();
    auto source = std::make_shared<Source>();
    const auto online = Track("user-companion");
    const auto local = CacheAudio(root, online, source);
    const auto cover = local.stream_url.substr(0, local.stream_url.rfind('.')) + ".cover.jpg";
    std::ofstream(cover, std::ios::binary) << Jpeg(false);
    Response(online, Jpeg());
    {
        MusicCoverLoader loader([](uint32_t, auto) {});
        loader.Request(1, local, source, root);
        assert(Wait([] { return Application::GetInstance().ScheduledCount() == 1; }));
        Application::GetInstance().Drain();
        std::ifstream file(cover, std::ios::binary);
        assert(std::string(std::istreambuf_iterator<char>(file), {}) == Jpeg(false));
        assert(!std::filesystem::exists(cover + ".meta"));
    }
    FinishWorkers();
    Reset();
    MusicTrack manual = online;
    manual.id = manual.stream_url = root + "/manual.mp3";
    std::ofstream(manual.stream_url) << "ID3saved";
    // Even a remote URL supplied on an unmanaged local track does not enable HTTP.
    Response(online, Jpeg());
    {
        MusicCoverLoader loader([](uint32_t, auto) { std::abort(); });
        loader.Request(2, manual, source, root);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    FinishWorkers();
    assert(Opens() == 0 && Application::GetInstance().ScheduledCount() == 0);
    assert(MusicCache(root).CoverPath(manual, source->base_url()).empty());
}

void FailedResponse(const MusicTrack& track, int status) {
    std::lock_guard<std::mutex> lock(network_state.mutex);
    network_state.responses[track.cover_url] = {"", status, ""};
}

void TransientRecovery(const std::string& root) {
    Reset();
    const auto track = Track("retry");
    FailedResponse(track, 502);
    {
        MusicCoverLoader loader([](uint32_t, auto) {});
        loader.Request(1, track, std::make_shared<Source>(), root);
        assert(Wait([] { return Opens() == 1 && IdleHttp(); }));
        Response(track, Jpeg());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        assert(Opens() == 1);  // No immediate busy loop after a transient failure.
        fake_time += 3000000;
        assert(Wait([] { return Application::GetInstance().ScheduledCount() == 1; }));
        Application::GetInstance().Drain();
        assert(Opens() == 2);
    }
    FinishWorkers();
}

void RetryBoundedAndNotFound(const std::string& root) {
    Reset();
    const auto track = Track("retry-bounded");
    FailedResponse(track, 503);
    {
        MusicCoverLoader loader([](uint32_t, auto) { std::abort(); });
        loader.Request(1, track, std::make_shared<Source>(), root);
        for (size_t attempt = 1; attempt <= 3; ++attempt) {
            assert(Wait([&] { return Opens() == attempt && IdleHttp(); }));
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            fake_time += 3000000;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        assert(Opens() == 3);
    }
    FinishWorkers();
    Reset();
    FailedResponse(track, 404);
    {
        MusicCoverLoader loader([](uint32_t, auto) { std::abort(); });
        loader.Request(2, track, std::make_shared<Source>(), root);
        assert(Wait([] { return Opens() == 1 && IdleHttp(); }));
        fake_time += 60000000;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        assert(Opens() == 1);
    }
    FinishWorkers();
}

void RetryVoiceAndNextTrack(const std::string& root) {
    Reset();
    const auto first = Track("retry-cancel");
    const auto second = Track("retry-next");
    FailedResponse(first, 502);
    Response(second, Jpeg());
    std::vector<uint32_t> delivered;
    {
        MusicCoverLoader loader([&](uint32_t session, auto) { delivered.push_back(session); });
        loader.Request(1, first, std::make_shared<Source>(), root);
        assert(Wait([] { return Opens() == 1 && IdleHttp(); }));
        Application::GetInstance().mode = kDeviceStateListening;
        fake_time += 60000000;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        assert(Opens() == 1);
        loader.Request(2, second, std::make_shared<Source>(), root);
        Application::GetInstance().mode = kDeviceStatePlaying;
        assert(Wait([] { return Application::GetInstance().ScheduledCount() == 1; }));
        Application::GetInstance().Drain();
        assert(Opens() == 2 && (delivered == std::vector<uint32_t>{2}));
    }
    FinishWorkers();
}

void DestroyDuringIo(const std::string& root) {
    Reset();
    auto track = Track("destroy");
    Response(track, Jpeg());
    network_state.block_reads = true;
    int delivered = 0;
    auto loader = std::make_unique<MusicCoverLoader>([&](uint32_t, auto) { ++delivered; });
    loader->Request(1, track, std::make_shared<Source>(), root);
    assert(Wait([] { return Opens() == 1; }));
    loader.reset();
    ReleaseReads();
    FinishWorkers();
    Application::GetInstance().Drain();
    assert(delivered == 0 && live_images == 0);
}

void DestroyAfterSchedule(const std::string& root) {
    Reset();
    auto track = Track("queued");
    Response(track, Jpeg());
    auto lifetime = std::make_shared<int>(0);
    std::weak_ptr<int> weak = lifetime;
    int delivered = 0;
    auto loader = std::make_unique<MusicCoverLoader>([weak, &delivered](uint32_t, auto) {
        if (!weak.expired())
            ++delivered;
    });
    loader->Request(1, track, std::make_shared<Source>(), root);
    assert(Wait([] { return Application::GetInstance().ScheduledCount() == 1; }));
    assert(live_images == 1);
    // Match CustomLcdDisplay's lifetime invalidation before loader destruction.
    lifetime.reset();
    loader.reset();
    FinishWorkers();
    Application::GetInstance().Drain();
    assert(delivered == 0 && live_images == 0);
}

void InvalidImageNotCached(const std::string& root) {
    Reset();
    auto track = Track("invalid");
    Response(track, Jpeg(false));
    auto source = std::make_shared<Source>();
    {
        MusicCoverLoader loader([](uint32_t, auto) { std::abort(); });
        loader.Request(1, track, source, root);
        assert(Wait([] { return decoder_calls == 1 && IdleHttp(); }));
        fake_time += 60000000;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        assert(Opens() == 1 && decoder_calls == 1);
    }
    FinishWorkers();
    assert(!std::filesystem::exists(MusicCache(root).CoverPath(track, source->base_url())));
    assert(Application::GetInstance().ScheduledCount() == 0 && live_images == 0);
}

void OversizedImageNotDecoded(const std::string& root) {
    Reset();
    auto track = Track("oversized");
    Response(track, std::string(65 * 1024, 'x'));
    auto source = std::make_shared<Source>();
    {
        MusicCoverLoader loader([](uint32_t, auto) { std::abort(); });
        loader.Request(1, track, source, root);
        assert(Wait([] { return Opens() == 1 && IdleHttp(); }));
        fake_time += 60000000;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        assert(Opens() == 1);
    }
    FinishWorkers();
    assert(network_state.reads == 65 && decoder_calls == 0);
    assert(!std::filesystem::exists(MusicCache(root).CoverPath(track, source->base_url())));
    assert(Application::GetInstance().ScheduledCount() == 0 && live_images == 0);
}

void DeadlineAndRedirect(const std::string& root) {
    Reset();
    auto track = Track("slow");
    Response(track, std::string(5000, 'x'));
    network_state.advance_per_read = 3000000;
    {
        MusicCoverLoader loader([](uint32_t, auto) { std::abort(); });
        loader.Request(1, track, std::make_shared<Source>(), root);
        assert(Wait([] { return Opens() == 1 && IdleHttp(); }));
    }
    FinishWorkers();
    assert(network_state.reads == 3 && decoder_calls == 0);
    Reset();
    track = Track("redirect");
    network_state.responses[track.cover_url] = {"", 302, "//cdn.example/cover.jpg"};
    network_state.responses["https://cdn.example/cover.jpg"] = {Jpeg(), 200, ""};
    int delivered = 0;
    {
        MusicCoverLoader loader([&](uint32_t, auto) { ++delivered; });
        loader.Request(2, track, std::make_shared<Source>(), root);
        assert(Wait([] { return Application::GetInstance().ScheduledCount() == 1; }));
        Application::GetInstance().Drain();
        assert(delivered == 1 && Opens() == 2);
    }
    FinishWorkers();
}

int main(int argc, char** argv) {
    assert(argc == 2 || argc == 3);
    for (const auto& test : std::vector<std::pair<std::string, void (*)(const std::string&)>>{
             {"voice", InterruptedVoice},
             {"latest", LatestWins},
             {"corrupt", CorruptCache},
             {"local_missing", LocalMissingCover},
             {"local_corrupt", LocalCorruptCover},
             {"local_user", LocalUserCover},
             {"retry", TransientRecovery},
             {"retry_bounded", RetryBoundedAndNotFound},
             {"retry_cancel", RetryVoiceAndNextTrack},
             {"destroy_io", DestroyDuringIo},
             {"destroy_queued", DestroyAfterSchedule},
             {"invalid", InvalidImageNotCached},
             {"oversized", OversizedImageNotDecoded},
             {"deadline_redirect", DeadlineAndRedirect}}) {
        if (argc == 3 && test.first != argv[2])
            continue;
        const auto root = std::string(argv[1]) + "/" + test.first;
        std::filesystem::create_directories(root);
        test.second(root);
        std::cout << test.first << " passed\n";
    }
    assert(live_images == 0 && workers == 0);
}
