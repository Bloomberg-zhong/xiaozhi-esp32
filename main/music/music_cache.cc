#include "music_cache.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef ESP_PLATFORM
#include <esp_vfs_fat.h>
#else
#include <sys/statvfs.h>
#endif

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <utility>
#include <vector>

#include "music_util.h"

namespace {
constexpr const char* kMarker = "XIAOZHI-MUSIC-CACHE-1\n";
constexpr size_t kMaxFieldBytes = 64 * 1024;
constexpr const char* kLyricMarker = "XIAOZHI-MUSIC-LYRICS-1\n";
std::mutex cache_mutex;
std::map<std::string, size_t> pins;
std::map<std::string, size_t> artwork_pins;
std::map<std::string, uint64_t> writers;

struct Metadata {
    MusicTrack track;
    std::string guard;
    uint64_t bytes = 0;
    uint32_t checksum = 0;
};

uint32_t Checksum(uint32_t current, const char* data, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        current = (current ^ static_cast<uint8_t>(data[i])) * 16777619U;
    }
    return current;
}

std::string Key(const MusicTrack& track, const std::string& base) {
    uint64_t hash = 14695981039346656037ULL;
    for (const auto& part :
         {base, track.provider, track.id.empty() ? track.stream_url : track.id}) {
        const std::string field = std::to_string(part.size()) + ":" + part;
        for (unsigned char c : field) {
            hash = (hash ^ c) * 1099511628211ULL;
        }
    }
    char key[17];
    std::snprintf(key, sizeof(key), "%016llx", static_cast<unsigned long long>(hash));
    return key;
}

std::string SafeName(std::string name) {
    for (char& c : name) {
        if (static_cast<unsigned char>(c) < 32 || std::strchr("/\\:*?\"<>|", c)) {
            c = '_';
        }
    }
    if (name.size() > 96) {
        size_t end = 96;
        while (end > 0 && (static_cast<unsigned char>(name[end]) & 0xc0) == 0x80) {
            --end;
        }
        name.resize(end);
    }
    return name.empty() ? "Song" : name;
}

bool RegularFile(const std::string& path, struct stat& info) {
    return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

bool FreeBytes(const std::string& root, uint64_t& bytes) {
#ifdef ESP_PLATFORM
    uint64_t total = 0;
    return esp_vfs_fat_info(root.c_str(), &total, &bytes) == ESP_OK;
#else
    struct statvfs info;
    if (statvfs(root.c_str(), &info) != 0) {
        return false;
    }
    bytes = static_cast<uint64_t>(info.f_bavail) * info.f_frsize;
    return true;
#endif
}

bool WriteField(FILE* file, const std::string& value) {
    return value.size() <= kMaxFieldBytes &&
           std::fprintf(file, "%u\n", unsigned(value.size())) > 0 &&
           std::fwrite(value.data(), 1, value.size(), file) == value.size() &&
           std::fputc('\n', file) != EOF;
}

bool ReadField(FILE* file, std::string& value) {
    unsigned size = 0;
    if (std::fscanf(file, "%u", &size) != 1 || std::fgetc(file) != '\n' || size > kMaxFieldBytes) {
        return false;
    }
    value.resize(size);
    return std::fread(value.data(), 1, size, file) == size && std::fgetc(file) == '\n';
}

bool ReadMetadata(const std::string& path, Metadata& metadata) {
    FILE* file = std::fopen((path + ".meta").c_str(), "rb");
    if (!file) {
        return false;
    }
    char marker[32] = {};
    unsigned long long bytes = 0;
    unsigned checksum = 0, duration = 0;
    bool ok = std::fgets(marker, sizeof(marker), file) && std::strcmp(marker, kMarker) == 0 &&
              std::fscanf(file, "%llu %u %u", &bytes, &checksum, &duration) == 3 &&
              std::fgetc(file) == '\n' && ReadField(file, metadata.guard) &&
              ReadField(file, metadata.track.title) && ReadField(file, metadata.track.artist) &&
              ReadField(file, metadata.track.album) && ReadField(file, metadata.track.cover_url) &&
              ReadField(file, metadata.track.lyric_text) && std::fgetc(file) == EOF;
    std::fclose(file);
    metadata.bytes = bytes;
    metadata.checksum = checksum;
    metadata.track.duration_ms = duration;
    metadata.track.id = metadata.track.stream_url = path;
    const std::string partial = "/.xzcache-" + metadata.guard + ".part";
    const bool partial_name =
        path.size() >= partial.size() &&
        path.compare(path.size() - partial.size(), partial.size(), partial) == 0;
    return ok && bytes > 0 && metadata.guard.size() == 16 &&
           (path.find(" [xz-" + metadata.guard + "].") != std::string::npos || partial_name);
}

bool ValidAudio(const std::string& path, const Metadata& metadata) {
    struct stat info;
    if (!RegularFile(path, info) || static_cast<uint64_t>(info.st_size) != metadata.bytes) {
        return false;
    }
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) {
        return false;
    }
    // This path is also called by the cover and local scanner tasks. Keep the
    // checksum workspace off their small stacks, including libc/FAT call depth.
    std::vector<char> buffer(2048);
    uint32_t checksum = 2166136261U;
    size_t read;
    while ((read = std::fread(buffer.data(), 1, buffer.size(), file)) > 0) {
        checksum = Checksum(checksum, buffer.data(), read);
    }
    bool ok = !std::ferror(file) && checksum == metadata.checksum;
    std::fclose(file);
    return ok;
}

std::string Guard(const std::string& root, const std::string& key) {
    return root + "/music-cache/" + key;
}

std::vector<std::string> List(const std::string& folder) {
    std::vector<std::string> paths;
    DIR* dir = opendir(folder.c_str());
    if (!dir) {
        return paths;
    }
    while (auto* entry = readdir(dir)) {
        if (std::strcmp(entry->d_name, ".") && std::strcmp(entry->d_name, "..")) {
            paths.push_back(folder + "/" + entry->d_name);
        }
    }
    closedir(dir);
    return paths;
}

bool EndsWith(const std::string& text, const char* suffix) {
    size_t len = std::strlen(suffix);
    return text.size() >= len && text.compare(text.size() - len, len, suffix) == 0;
}

struct PendingJob {
    MusicTrack track;
    std::string base, guard;
    uint64_t expected = 0, written = 0;
    uint32_t checksum = 0;
};

bool ReadPending(const std::string& path, PendingJob& job) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file)
        return false;
    char marker[32] = {};
    unsigned long long expected = 0, written = 0;
    unsigned duration = 0, checksum = 0;
    bool ok =
        std::fgets(marker, sizeof(marker), file) &&
        std::strcmp(marker, "XIAOZHI-MUSIC-PENDING-1\n") == 0 &&
        std::fscanf(file, "%llu %llu %u %u", &expected, &written, &checksum, &duration) == 4 &&
        std::fgetc(file) == '\n' && ReadField(file, job.base) && ReadField(file, job.guard) &&
        ReadField(file, job.track.id) && ReadField(file, job.track.provider) &&
        ReadField(file, job.track.title) && ReadField(file, job.track.artist) &&
        ReadField(file, job.track.album) && ReadField(file, job.track.lyric_text) &&
        std::fgetc(file) == EOF;
    std::fclose(file);
    job.expected = expected;
    job.written = written;
    job.checksum = checksum;
    job.track.duration_ms = duration;
    return ok && expected > 0 && written <= expected && !job.track.id.empty() &&
           job.guard.size() == 16 &&
           (EndsWith(path, (".xzcache-" + job.guard + ".part.pending").c_str()) ||
            EndsWith(path, (".xzcache-" + job.guard + ".part.meta").c_str()));
}

// A failed metadata publication can leave the newest valid resume record in
// .part.meta. Prefer it over an older .pending record whose length is stale.
bool ReadAvailablePending(const std::string& part, PendingJob& job) {
    struct stat info;
    if (!RegularFile(part, info))
        return false;
    for (const char* suffix : {".meta", ".pending"}) {
        if (ReadPending(part + suffix, job) && static_cast<uint64_t>(info.st_size) == job.written)
            return true;
    }
    return false;
}

void RemovePendingMarkers(const std::string& part) {
    for (const char* suffix : {".meta", ".pending"}) {
        PendingJob job;
        if (ReadPending(part + suffix, job))
            std::remove((part + suffix).c_str());
    }
}

bool OwnedCover(const std::string& path, std::string& key) {
    if (!EndsWith(path, ".cover.jpg"))
        return false;
    auto pos = path.rfind(" [xz-");
    if (pos == std::string::npos || pos + 5 + 16 + 11 != path.size())
        return false;
    key = path.substr(pos + 5, 16);
    FILE* marker = std::fopen((path + ".meta").c_str(), "rb");
    if (!marker)
        return false;
    char text[64] = {};
    bool ok = std::fgets(text, sizeof(text), marker) &&
              std::strcmp(text, "XIAOZHI-MUSIC-COVER-1\n") == 0 && std::fgetc(marker) == EOF;
    std::fclose(marker);
    return ok;
}

bool OwnedLyrics(const std::string& path, std::string& key, uint64_t& bytes, uint32_t& checksum) {
    if (!EndsWith(path, ".lrc"))
        return false;
    const auto pos = path.rfind(" [xz-");
    if (pos == std::string::npos || pos + 5 + 16 + 5 != path.size() ||
        path.compare(pos + 21, 5, "].lrc") != 0)
        return false;
    FILE* marker = std::fopen((path + ".meta").c_str(), "rb");
    if (!marker)
        return false;
    char text[32] = {};
    unsigned long long length = 0;
    unsigned digest = 0;
    const bool ok =
        std::fgets(text, sizeof(text), marker) && std::strcmp(text, kLyricMarker) == 0 &&
        std::fscanf(marker, "%llu %u", &length, &digest) == 2 && std::fgetc(marker) == '\n' &&
        std::fgetc(marker) == EOF && length > 0 && length <= kMaxFieldBytes;
    std::fclose(marker);
    key = path.substr(pos + 5, 16);
    bytes = length;
    checksum = digest;
    return ok;
}

bool ReadLyricPayload(const std::string& path, std::string& text) {
    std::string key;
    uint64_t bytes = 0;
    uint32_t checksum = 0;
    struct stat info;
    if (!OwnedLyrics(path, key, bytes, checksum) || !RegularFile(path, info) ||
        static_cast<uint64_t>(info.st_size) != bytes)
        return false;
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file)
        return false;
    std::string body(static_cast<size_t>(bytes), '\0');
    const bool ok = std::fread(body.data(), 1, body.size(), file) == body.size() &&
                    std::fgetc(file) == EOF && !std::ferror(file) &&
                    Checksum(2166136261U, body.data(), body.size()) == checksum;
    std::fclose(file);
    if (ok)
        text = std::move(body);
    return ok;
}

bool PathExists(const std::string& path) {
    struct stat info;
#ifdef ESP_PLATFORM
    // FAT VFS has no symbolic links and ESP-IDF does not export lstat.
    return stat(path.c_str(), &info) == 0;
#else
    return lstat(path.c_str(), &info) == 0;
#endif
}

bool ReadOwnedLocalAudio(const std::string& root, const MusicTrack& track, Metadata& metadata) {
    const std::string folder = root + "/music-cache";
    const std::string prefix = folder + "/";
    const auto& path = track.stream_url;
    // Titles/artists may contain '#', which is a URL fragment delimiter but
    // ordinary filename text. Detect only the final filesystem extension.
    const auto dot = path.rfind('.');
    const std::string extension = dot == std::string::npos ? "" : path.substr(dot);
    if (root.empty() || path.compare(0, prefix.size(), prefix) != 0 ||
        path.find_first_of("/\\", prefix.size()) != std::string::npos ||
        extension.find_first_of("?#") != std::string::npos ||
        DetectMusicAudioFormat(nullptr, 0, "", extension) == MusicAudioFormat::kUnknown)
        return false;
    struct stat audio, marker;
#ifdef ESP_PLATFORM
    // FAT has no symbolic links. Host fixtures also reject links that could
    // redirect an apparently managed audio/metadata pair outside this cache.
    if (!RegularFile(path, audio) || !RegularFile(path + ".meta", marker))
        return false;
#else
    struct stat directory;
    if (lstat(folder.c_str(), &directory) != 0 || !S_ISDIR(directory.st_mode) ||
        lstat(path.c_str(), &audio) != 0 || !S_ISREG(audio.st_mode) ||
        lstat((path + ".meta").c_str(), &marker) != 0 || !S_ISREG(marker.st_mode))
        return false;
#endif
    return ReadMetadata(path, metadata);
}

bool CoverTarget(const std::string& root, const MusicTrack& track, const std::string& base,
                 std::string& path, std::string& key) {
    if (root.empty() || track.live)
        return false;
    if (!track.stream_url.empty() && track.stream_url.front() == '/') {
        Metadata metadata;
        if (!ReadOwnedLocalAudio(root, track, metadata))
            return false;
        key = metadata.guard;
        path = track.stream_url.substr(0, track.stream_url.rfind('.')) + ".cover.jpg";
    } else {
        if (track.id.empty())
            return false;
        key = Key(track, base);
        path = root + "/music-cache/" + SafeName(track.artist + " - " + track.title) + " [xz-" +
               key + "].cover.jpg";
    }
    return true;
}

// Callers validate ownership of every existing destination. FAT cannot rename
// over a destination, so stage old files aside and roll back the entire pair
// if publication fails. Never clobber a preexisting backup or user temporary.
bool PublishOwnedFiles(std::vector<std::pair<std::string, std::string>> files) {
    std::vector<bool> backed_up(files.size(), false), published(files.size(), false);
    for (const auto& file : files) {
        if (!PathExists(file.first) || PathExists(file.second + ".bak"))
            return false;
    }
    const auto rollback = [&] {
        for (size_t index = files.size(); index-- > 0;) {
            const auto& file = files[index];
            if (published[index] && std::rename(file.second.c_str(), file.first.c_str()) != 0) {
                // This destination was published by this transaction only.
                std::remove(file.second.c_str());
            }
            if (backed_up[index])
                std::rename((file.second + ".bak").c_str(), file.second.c_str());
        }
    };
    for (size_t index = 0; index < files.size(); ++index) {
        const auto& file = files[index];
        if (PathExists(file.second)) {
            if (std::rename(file.second.c_str(), (file.second + ".bak").c_str()) != 0) {
                rollback();
                return false;
            }
            backed_up[index] = true;
        }
    }
    for (size_t index = 0; index < files.size(); ++index) {
        const auto& file = files[index];
        if (std::rename(file.first.c_str(), file.second.c_str()) != 0) {
            rollback();
            return false;
        }
        published[index] = true;
    }
    for (size_t index = 0; index < files.size(); ++index) {
        if (backed_up[index])
            std::remove((files[index].second + ".bak").c_str());
    }
    return true;
}

bool MakeRoom(const std::string& root, MusicCache::Limits limits, uint64_t incoming,
              const std::string& own_guard, const std::string& replacing = "") {
    struct Candidate {
        std::string path;
        uint64_t bytes;
        time_t time;
        uint64_t reserved = 0;
        bool pending = false;
    };
    std::vector<Candidate> removable;
    std::set<std::string> counted_pending;
    uint64_t used = 0, reserved_free = 0;
    const std::string folder = root + "/music-cache";
    for (const auto& path : List(folder)) {
        if (EndsWith(path, ".pending") || EndsWith(path, ".part.meta")) {
            const std::string part =
                path.substr(0, path.size() - (EndsWith(path, ".pending") ? 8 : 5));
            PendingJob pending;
            struct stat info;
            if (ReadAvailablePending(part, pending) && counted_pending.insert(part).second) {
                const auto guard = Guard(root, pending.guard);
                if (guard != own_guard && !writers.count(guard) && RegularFile(part, info)) {
                    used += pending.expected;
                    const auto reserved = pending.expected - pending.written;
                    reserved_free += reserved;
                    if (!pins.count(guard) && !artwork_pins.count(guard))
                        removable.push_back(
                            {part, pending.expected, info.st_mtime, reserved, true});
                }
            }
        } else if (EndsWith(path, ".meta")) {
            const std::string audio = path.substr(0, path.size() - 5);
            Metadata metadata;
            struct stat info;
            std::string cover_key;
            uint64_t lyric_bytes = 0;
            uint32_t lyric_checksum = 0;
            if (OwnedCover(audio, cover_key) && RegularFile(audio, info)) {
                if (audio == replacing)
                    continue;
                used += info.st_size;
                const auto guard = Guard(root, cover_key);
                if (!pins.count(guard) && !artwork_pins.count(guard) && !writers.count(guard)) {
                    removable.push_back(
                        {audio, static_cast<uint64_t>(info.st_size), info.st_mtime});
                }
            } else if (OwnedLyrics(audio, cover_key, lyric_bytes, lyric_checksum) &&
                       RegularFile(audio, info)) {
                if (audio == replacing)
                    continue;
                used += info.st_size;
                const auto guard = Guard(root, cover_key);
                if (!pins.count(guard) && !writers.count(guard)) {
                    removable.push_back(
                        {audio, static_cast<uint64_t>(info.st_size), info.st_mtime});
                }
            } else if (ReadMetadata(audio, metadata) && RegularFile(audio, info)) {
                used += info.st_size;
                if (!pins.count(Guard(root, metadata.guard))) {
                    removable.push_back(
                        {audio, static_cast<uint64_t>(info.st_size), info.st_mtime});
                }
            }
        }
    }
    std::sort(removable.begin(), removable.end(), [](const Candidate& a, const Candidate& b) {
        return a.time < b.time || (a.time == b.time && a.path < b.path);
    });
    // Reserve the complete size of every live writer, including bytes not yet
    // written; no two tasks can overbook the budget/free-space headroom.
    for (const auto& active : writers) {
        if (active.first.compare(0, folder.size() + 1, folder + "/") == 0 &&
            active.first != own_guard) {
            used += active.second;
            struct stat info;
            auto part =
                folder + "/.xzcache-" + active.first.substr(active.first.size() - 16) + ".part";
            uint64_t written = RegularFile(part, info) ? info.st_size : 0;
            reserved_free += active.second - std::min(written, active.second);
        }
    }
    uint64_t free = 0;
    for (size_t next = 0;; ++next) {
        bool enough = incoming <= limits.max_bytes && used <= limits.max_bytes - incoming &&
                      FreeBytes(root, free) && free >= limits.min_free_bytes &&
                      reserved_free <= free - limits.min_free_bytes &&
                      incoming <= free - limits.min_free_bytes - reserved_free;
        if (enough) {
            return true;
        }
        if (next >= removable.size()) {
            return false;
        }
        const auto& entry = removable[next];
        if (std::remove(entry.path.c_str()) == 0) {
            used -= entry.bytes;
            if (entry.pending) {
                reserved_free -= entry.reserved;
                RemovePendingMarkers(entry.path);
            } else {
                std::remove((entry.path + ".meta").c_str());
            }
        }
    }
}
}  // namespace

MusicCache::MusicCache(std::string root) : MusicCache(std::move(root), Limits{}) {}
MusicCache::MusicCache(std::string root, Limits limits) : root_(std::move(root)), limits_(limits) {
    while (root_.size() > 1 && root_.back() == '/') {
        root_.pop_back();
    }
}
MusicCache::Entry::~Entry() {
    std::lock_guard<std::mutex> lock(cache_mutex);
    auto& holders = artwork_only ? artwork_pins : pins;
    if (--holders[guard] == 0) {
        holders.erase(guard);
    }
}

bool MusicCache::IsManagedPath(const std::string& path) {
    return path.find("/music-cache/") != std::string::npos &&
           path.find(" [xz-") != std::string::npos;
}

bool MusicCache::ReadTrack(const std::string& path, MusicTrack& track) {
    Metadata metadata;
    if (!ReadMetadata(path, metadata) || !ValidAudio(path, metadata)) {
        return false;
    }
    track = std::move(metadata.track);
    struct stat info;
    const auto stem = path.substr(0, path.rfind('.'));
    if (RegularFile(stem + ".cover.jpg", info))
        track.cover_url = stem + ".cover.jpg";
    const auto lyric = stem + ".lrc";
    if (RegularFile(lyric, info)) {
        // User companions remain readable; managed downloads must pass their
        // own integrity check before an offline reader uses them.
        std::string text;
        if (!PathExists(lyric + ".meta") || ReadLyricPayload(lyric, text))
            track.lyric_url = lyric;
    }
    return true;
}

std::shared_ptr<MusicCache::Entry> MusicCache::PinLocal(const std::string& path) const {
    std::lock_guard<std::mutex> lock(cache_mutex);
    MusicTrack track;
    if (root_.empty() || !IsManagedPath(path) || !ReadTrack(path, track)) {
        return nullptr;
    }
    Metadata metadata;
    if (!ReadMetadata(path, metadata)) {
        return nullptr;
    }
    auto entry = std::make_shared<Entry>();
    entry->track = std::move(track);
    entry->guard = Guard(root_, metadata.guard);
    ++pins[entry->guard];
    return entry;
}

std::shared_ptr<MusicCache::Entry> MusicCache::Find(const MusicTrack& track,
                                                    const std::string& base) const {
    if (root_.empty() || track.live) {
        return nullptr;
    }
    const auto key = Key(track, base);
    for (const auto& path : List(root_ + "/music-cache")) {
        if (path.find(" [xz-" + key + "].") != std::string::npos && !EndsWith(path, ".meta")) {
            if (auto entry = PinLocal(path)) {
                return entry;
            }
        }
    }
    return nullptr;
}

bool MusicCache::SameIdentity(const MusicTrack& left, const std::string& left_base,
                              const MusicTrack& right, const std::string& right_base) {
    return Key(left, left_base) == Key(right, right_base);
}

bool MusicCache::IsWriting(const MusicTrack& track, const std::string& base) const {
    std::lock_guard<std::mutex> lock(cache_mutex);
    return writers.count(Guard(root_, Key(track, base))) != 0;
}

std::shared_ptr<MusicCache::Entry> MusicCache::PinArtwork(const MusicTrack& track,
                                                          const std::string& base) const {
    if (root_.empty() || track.live)
        return nullptr;
    std::lock_guard<std::mutex> lock(cache_mutex);
    std::string key = Key(track, base);
    if (!track.stream_url.empty() && track.stream_url.front() == '/') {
        Metadata metadata;
        if (!ReadOwnedLocalAudio(root_, track, metadata))
            return nullptr;
        key = metadata.guard;
    }
    auto pin = std::make_shared<Entry>();
    pin->track = track;
    pin->guard = Guard(root_, key);
    pin->artwork_only = true;
    ++artwork_pins[pin->guard];
    // Artwork is independently decoded; it must not checksum the entire audio
    // file a second time merely to protect its companion from garbage collection.
    return pin;
}

std::unique_ptr<MusicCache::Writer> MusicCache::Begin(const MusicTrack& track,
                                                      const std::string& base,
                                                      uint64_t expected) const {
    if (root_.empty() || track.live || expected == 0 || expected > limits_.max_bytes) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(cache_mutex);
    struct stat info;
    if (stat(root_.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) {
        return nullptr;
    }
    std::string folder = root_ + "/music-cache";
    if (mkdir(folder.c_str(), 0775) != 0 &&
        (stat(folder.c_str(), &info) != 0 || !S_ISDIR(info.st_mode))) {
        return nullptr;
    }
    const auto key = Key(track, base);
    const auto guard = Guard(root_, key);
    // Find and Begin can straddle another task's atomic publication. Recheck
    // while holding the writer lock so a completed song is never rewritten.
    for (const auto& path : List(folder)) {
        MusicTrack existing_track;
        if (path.find(" [xz-" + key + "].") != std::string::npos && !EndsWith(path, ".meta") &&
            ReadTrack(path, existing_track)) {
            return nullptr;
        }
    }
    PendingJob pending;
    const std::string part = folder + "/.xzcache-" + key + ".part";
    PendingJob owner;
    if (PathExists(part) && !(ReadPending(part + ".meta", owner) && owner.guard == key) &&
        !(ReadPending(part + ".pending", owner) && owner.guard == key))
        return nullptr;  // Never truncate an unmarked file at a reserved-looking path.
    uint64_t existing = 0;
    if (ReadAvailablePending(part, pending) && pending.expected == expected &&
        RegularFile(part, info) && static_cast<uint64_t>(info.st_size) == pending.written) {
        existing = info.st_size;
    }
    if (writers.count(guard) || pins.count(guard) || !MakeRoom(root_, limits_, expected, guard)) {
        return nullptr;
    }
    auto writer = std::make_unique<Writer>();
    writer->part_ = part;
    // A bounded prefix checksum is rebuilt on resume; no unbounded RAM buffers.
    if (existing) {
        FILE* input = std::fopen(part.c_str(), "rb");
        if (!input)
            return nullptr;
        std::vector<char> bytes(2048);
        size_t size;
        while ((size = std::fread(bytes.data(), 1, bytes.size(), input)) > 0) {
            size_t prefix = std::min(size, sizeof(writer->header_) - writer->header_size_);
            std::memcpy(writer->header_ + writer->header_size_, bytes.data(), prefix);
            writer->header_size_ += prefix;
            writer->checksum_ = Checksum(writer->checksum_, bytes.data(), size);
            writer->written_ += size;
        }
        bool ok = !std::ferror(input) && writer->written_ == existing;
        std::fclose(input);
        if (!ok || writer->checksum_ != pending.checksum) {
            RemovePendingMarkers(part);
            std::remove(part.c_str());
            writer->written_ = 0;
            writer->header_size_ = 0;
            writer->checksum_ = 2166136261U;
            existing = 0;
        }
    }
    writer->file_ = std::fopen(writer->part_.c_str(), existing ? "ab" : "wb");
    if (!writer->file_) {
        return nullptr;
    }
    writer->root_ = root_;
    writer->base_ = base;
    writer->guard_ = guard;
    writer->stem_ =
        folder + "/" + SafeName(track.artist + " - " + track.title) + " [xz-" + key + "]";
    writer->track_ = track;
    writer->expected_ = expected;
    writer->min_free_bytes_ = limits_.min_free_bytes;
    writers[guard] = expected;
    return writer;
}

MusicCache::Writer::~Writer() { Abort(); }
void MusicCache::Writer::Abort() {
    if (file_) {
        std::fclose(file_);
        file_ = nullptr;
    }
    if (!guard_.empty()) {
        std::lock_guard<std::mutex> lock(cache_mutex);
        std::remove(part_.c_str());
        RemovePendingMarkers(part_);
        Metadata metadata;
        if (ReadMetadata(part_, metadata) && metadata.guard == guard_.substr(guard_.size() - 16))
            std::remove((part_ + ".meta").c_str());
        writers.erase(guard_);
        guard_.clear();
    }
}

bool MusicCache::Writer::Append(size_t offset, const char* data, size_t size) {
    uint64_t free = 0;
    if (!file_ || offset != written_ || size > expected_ - written_ || !FreeBytes(root_, free) ||
        free < min_free_bytes_ || size > free - min_free_bytes_ ||
        std::fwrite(data, 1, size, file_) != size) {
        Abort();
        return false;
    }
    const size_t prefix = std::min(size, sizeof(header_) - header_size_);
    std::memcpy(header_ + header_size_, data, prefix);
    header_size_ += prefix;
    checksum_ = Checksum(checksum_, data, size);
    written_ += size;
    return true;
}

bool MusicCache::Writer::Finish() {
    if (!file_ || written_ != expected_) {
        Abort();
        return false;
    }
    const auto format = DetectMusicAudioFormat(header_, header_size_, "", "");
    if (format == MusicAudioFormat::kUnknown) {
        Abort();
        return false;
    }
    bool ok = std::fflush(file_) == 0;
    ok = std::fclose(file_) == 0 && ok;
    file_ = nullptr;
    const std::string audio = stem_ + "." + MusicAudioFormatName(format);
    PendingJob previous;
    Metadata old_metadata;
    const auto key = guard_.substr(guard_.size() - 16);
    if (PathExists(part_ + ".meta") &&
        !(ReadPending(part_ + ".meta", previous) && previous.guard == key) &&
        !(ReadMetadata(part_, old_metadata) && old_metadata.guard == key)) {
        Abort();
        return false;
    }
    FILE* meta = std::fopen((part_ + ".meta").c_str(), "wb");
    if (meta) {
        ok = std::fputs(kMarker, meta) >= 0 &&
             std::fprintf(meta, "%llu %u %u\n", static_cast<unsigned long long>(written_),
                          static_cast<unsigned>(checksum_),
                          static_cast<unsigned>(track_.duration_ms)) > 0 &&
             WriteField(meta, guard_.substr(guard_.size() - 16)) &&
             WriteField(meta, track_.title) && WriteField(meta, track_.artist) &&
             WriteField(meta, track_.album) && WriteField(meta, track_.cover_url) &&
             WriteField(meta, track_.lyric_text) && ok;
        ok = std::fclose(meta) == 0 && ok;
    } else {
        ok = false;
    }
    // Metadata first: an audio file is visible to the scanner only once both
    // final names exist and its full length/checksum agree with the marker.
    bool publication_failed = false;
    if (ok) {
        std::lock_guard<std::mutex> lock(cache_mutex);
        const bool owned = ReadMetadata(audio, old_metadata) && old_metadata.guard == key;
        if (!(PathExists(audio) || PathExists(audio + ".meta")) || owned) {
            ok = PublishOwnedFiles({{part_ + ".meta", audio + ".meta"}, {part_, audio}});
            publication_failed = !ok;
        } else {
            ok = false;
        }
    }
    if (publication_failed) {
        struct stat info;
        if (ReadMetadata(part_, old_metadata) && old_metadata.guard == key &&
            old_metadata.bytes == written_ && old_metadata.checksum == checksum_ &&
            RegularFile(part_, info) && static_cast<uint64_t>(info.st_size) == written_) {
            file_ = std::fopen(part_.c_str(), "ab");
            if (file_) {
                // Rollback restored the complete payload. Retain a checkpoint
                // so the next writer can retry publication without downloading.
                Suspend();
                return false;
            }
        }
    }
    Abort();
    return ok;
}

std::vector<MusicTrack> MusicCache::Pending(const std::string& base) const {
    std::lock_guard<std::mutex> lock(cache_mutex);
    std::vector<MusicTrack> tracks;
    if (root_.empty())
        return tracks;
    std::set<std::string> seen;
    for (const auto& path : List(root_ + "/music-cache")) {
        PendingJob job;
        if (!EndsWith(path, ".pending") && !EndsWith(path, ".part.meta"))
            continue;
        const auto part = path.substr(0, path.size() - (EndsWith(path, ".pending") ? 8 : 5));
        if (tracks.size() < 32 && ReadAvailablePending(part, job) && seen.insert(part).second &&
            job.base == Key(MusicTrack{}, base) && !writers.count(Guard(root_, job.guard))) {
            tracks.push_back(std::move(job.track));
        }
    }
    return tracks;
}

bool MusicCache::Writer::Suspend() {
    if (!file_ || track_.id.empty()) {
        Abort();
        return false;  // URL-only songs cannot be reconstructed safely after restart.
    }
    bool ok = std::fflush(file_) == 0;
    ok = std::fclose(file_) == 0 && ok;
    file_ = nullptr;
    PendingJob previous;
    Metadata completed;
    const auto key = guard_.substr(guard_.size() - 16);
    if (PathExists(part_ + ".meta") &&
        !(ReadPending(part_ + ".meta", previous) && previous.guard == key) &&
        !(ReadMetadata(part_, completed) && completed.guard == key && completed.bytes == written_ &&
          completed.checksum == checksum_)) {
        Abort();
        return false;
    }
    FILE* pending = std::fopen((part_ + ".meta").c_str(), "wb");
    if (pending) {
        ok = std::fputs("XIAOZHI-MUSIC-PENDING-1\n", pending) >= 0 &&
             std::fprintf(pending, "%llu %llu %u %u\n", static_cast<unsigned long long>(expected_),
                          static_cast<unsigned long long>(written_),
                          static_cast<unsigned>(checksum_),
                          static_cast<unsigned>(track_.duration_ms)) > 0 &&
             WriteField(pending, Key(MusicTrack{}, base_)) &&
             WriteField(pending, guard_.substr(guard_.size() - 16)) &&
             WriteField(pending, track_.id) && WriteField(pending, track_.provider) &&
             WriteField(pending, track_.title) && WriteField(pending, track_.artist) &&
             WriteField(pending, track_.album) && WriteField(pending, track_.lyric_text) && ok;
        ok = std::fclose(pending) == 0 && ok;
    } else {
        ok = false;
    }
    if (ok) {
        std::lock_guard<std::mutex> lock(cache_mutex);
        size_t count = 0;
        std::set<std::string> seen;
        std::vector<std::pair<time_t, std::string>> removable;
        for (const auto& path : List(root_ + "/music-cache")) {
            PendingJob job;
            struct stat info;
            if (!EndsWith(path, ".pending") && !EndsWith(path, ".part.meta"))
                continue;
            const auto part = path.substr(0, path.size() - (EndsWith(path, ".pending") ? 8 : 5));
            if (part != part_ && ReadAvailablePending(part, job) && seen.insert(part).second &&
                RegularFile(part, info)) {
                ++count;
                const auto guard = Guard(root_, job.guard);
                if (guard != guard_ && !writers.count(guard) && !pins.count(guard) &&
                    !artwork_pins.count(guard)) {
                    removable.emplace_back(info.st_mtime, part);
                }
            }
        }
        // Retain at most 32 owned continuations. Under pressure, a newly
        // selected song replaces the oldest inactive partial.
        std::sort(removable.begin(), removable.end());
        for (const auto& candidate : removable) {
            if (count < 32)
                break;
            if (std::remove(candidate.second.c_str()) == 0) {
                RemovePendingMarkers(candidate.second);
                --count;
            }
        }
        if (count >= 32) {
            ok = false;
        }
        if (ok) {
            const bool owned = ReadPending(part_ + ".pending", previous) && previous.guard == key;
            if (PathExists(part_ + ".pending") && !owned) {
                ok = false;
            } else {
                ok = PublishOwnedFiles({{part_ + ".meta", part_ + ".pending"}});
                if (!ok) {
                    // Latest valid .part.meta remains discoverable after a
                    // failed publication, even if the restored marker is older.
                    writers.erase(guard_);
                    guard_.clear();
                    return false;
                }
            }
        }
        if (ok) {
            writers.erase(guard_);
            guard_.clear();  // Destructor preserves the complete, marked partial.
        }
    }
    if (!ok)
        Abort();
    return ok;
}

std::string MusicCache::CoverPath(const MusicTrack& track, const std::string& base) const {
    std::lock_guard<std::mutex> lock(cache_mutex);
    std::string path, key;
    return CoverTarget(root_, track, base, path, key) ? path : "";
}

std::string MusicCache::OriginalCoverUrl(const MusicTrack& track) const {
    if (track.live)
        return "";
    std::lock_guard<std::mutex> lock(cache_mutex);
    Metadata metadata;
    return ReadOwnedLocalAudio(root_, track, metadata) && IsHttpUrl(metadata.track.cover_url)
               ? metadata.track.cover_url
               : "";
}

bool MusicCache::StoreCover(const MusicTrack& track, const std::string& base, const char* data,
                            size_t size) const {
    if (!data || size == 0 || size > 64 * 1024)
        return false;
    std::lock_guard<std::mutex> lock(cache_mutex);
    std::string path, key;
    if (!CoverTarget(root_, track, base, path, key))
        return false;
    struct stat info;
    if (stat(root_.c_str(), &info) != 0 || !S_ISDIR(info.st_mode))
        return false;
    const auto folder = root_ + "/music-cache";
    if (mkdir(folder.c_str(), 0775) != 0 &&
        (stat(folder.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)))
        return false;
    std::vector<std::pair<time_t, std::string>> covers;
    for (const auto& file : List(folder)) {
        std::string key;
        if (OwnedCover(file, key) && RegularFile(file, info)) {
            covers.emplace_back(info.st_mtime, file);
        }
    }
    std::sort(covers.begin(), covers.end());
    std::string existing_key;
    const bool replacing = OwnedCover(path, existing_key);
    if ((!replacing && (PathExists(path) || PathExists(path + ".meta"))) ||
        PathExists(path + ".part") || PathExists(path + ".meta.part"))
        return false;
    bool slot = covers.size() < 64 || replacing;
    for (auto cover = covers.begin(); !slot && cover != covers.end(); ++cover) {
        std::string key;
        OwnedCover(cover->second, key);
        const auto guard = Guard(root_, key);
        if (cover->second == path || pins.count(guard) || artwork_pins.count(guard) ||
            writers.count(guard))
            continue;
        if (std::remove(cover->second.c_str()) == 0) {
            std::remove((cover->second + ".meta").c_str());
            slot = true;
            break;
        }
    }
    if (!slot)
        return false;
    const auto guard = Guard(root_, key);
    ++pins[guard];  // MakeRoom cannot remove this song while writing its artwork.
    // Artwork must count this song's entire active/pending audio reservation.
    // The pin protects it while only the final replacement counts in the budget.
    const bool room = MakeRoom(root_, limits_, size, "", replacing ? path : "");
    if (--pins[guard] == 0)
        pins.erase(guard);
    if (!room)
        return false;
    const int fd = open((path + ".part").c_str(), O_WRONLY | O_CREAT | O_EXCL, 0664);
    if (fd < 0)
        return false;
    FILE* file = fdopen(fd, "wb");
    if (!file) {
        close(fd);
        std::remove((path + ".part").c_str());
        return false;
    }
    bool ok = std::fwrite(data, 1, size, file) == size;
    ok = std::fclose(file) == 0 && ok;
    const int meta_fd = open((path + ".meta.part").c_str(), O_WRONLY | O_CREAT | O_EXCL, 0664);
    FILE* marker = meta_fd < 0 ? nullptr : fdopen(meta_fd, "wb");
    if (marker) {
        ok = std::fputs("XIAOZHI-MUSIC-COVER-1\n", marker) >= 0 && ok;
        ok = std::fclose(marker) == 0 && ok;
    } else {
        if (meta_fd >= 0)
            close(meta_fd);
        ok = false;
    }
    if (ok) {
        ok = PublishOwnedFiles({{path + ".meta.part", path + ".meta"}, {path + ".part", path}});
    }
    std::remove((path + ".part").c_str());
    if (meta_fd >= 0)
        std::remove((path + ".meta.part").c_str());
    return ok;
}

bool MusicCache::ReadLyrics(const MusicTrack& track, const std::string& base,
                            std::string& text) const {
    if (root_.empty() || track.live)
        return false;
    std::lock_guard<std::mutex> lock(cache_mutex);
    const auto key = Key(track, base);
    for (const auto& path : List(root_ + "/music-cache")) {
        if (EndsWith(path, (" [xz-" + key + "].lrc").c_str()) && ReadLyricPayload(path, text))
            return true;
    }
    return false;
}

bool MusicCache::StoreLyrics(const MusicTrack& track, const std::string& base,
                             const std::vector<LyricLine>& lines) const {
    if (root_.empty() || track.live || lines.empty() || lines.size() > 512)
        return false;
    std::string text;
    for (const auto& line : lines) {
        if (line.text.find_first_of("\r\n") != std::string::npos ||
            line.text.find('\0') != std::string::npos)
            return false;
        char timestamp[32];
        const int length = std::snprintf(
            timestamp, sizeof(timestamp), "[%02u:%02u.%03u] ", unsigned(line.time_ms / 60000),
            unsigned(line.time_ms / 1000 % 60), unsigned(line.time_ms % 1000));
        if (length <= 0 || static_cast<size_t>(length) >= sizeof(timestamp) ||
            line.text.size() > kMaxFieldBytes - static_cast<size_t>(length) - 1 ||
            text.size() > kMaxFieldBytes - static_cast<size_t>(length) - line.text.size() - 1)
            return false;
        text.append(timestamp, length);
        text += line.text;
        text += '\n';
    }
    std::lock_guard<std::mutex> lock(cache_mutex);
    struct stat info;
    if (stat(root_.c_str(), &info) != 0 || !S_ISDIR(info.st_mode))
        return false;
    const auto folder = root_ + "/music-cache";
    if (mkdir(folder.c_str(), 0775) != 0 &&
        (stat(folder.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)))
        return false;
    const auto key = Key(track, base);
    auto path =
        folder + "/" + SafeName(track.artist + " - " + track.title) + " [xz-" + key + "].lrc";
    std::vector<std::pair<time_t, std::string>> lyrics;
    for (const auto& file : List(folder)) {
        std::string owner;
        uint64_t bytes = 0;
        uint32_t checksum = 0;
        if (OwnedLyrics(file, owner, bytes, checksum) && RegularFile(file, info)) {
            lyrics.emplace_back(info.st_mtime, file);
            if (owner == key)
                path = file;  // Reuse identity even if catalog title/artist changed.
        }
    }
    std::string owner;
    uint64_t bytes = 0;
    uint32_t checksum = 0;
    const bool owned = OwnedLyrics(path, owner, bytes, checksum) && owner == key;
    // Never overwrite a user's companion, marker, or preexisting temporary file.
    if ((!owned && (PathExists(path) || PathExists(path + ".meta"))) ||
        PathExists(path + ".part") || PathExists(path + ".meta.part"))
        return false;
    std::string existing;
    if (owned && ReadLyricPayload(path, existing) && existing == text)
        return true;
    std::sort(lyrics.begin(), lyrics.end());
    bool slot = lyrics.size() < 64 || owned;
    for (const auto& lyric : lyrics) {
        if (slot)
            break;
        OwnedLyrics(lyric.second, owner, bytes, checksum);
        const auto guard = Guard(root_, owner);
        if (pins.count(guard) || writers.count(guard))
            continue;
        if (std::remove(lyric.second.c_str()) == 0) {
            std::remove((lyric.second + ".meta").c_str());
            slot = true;
        }
    }
    if (!slot)
        return false;
    const auto guard = Guard(root_, key);
    ++pins[guard];
    // Count the full audio writer/pending reservation, including this song.
    // Replacement needs space for the atomic temporary, but only the final
    // companion contributes to the cache budget.
    const bool room = MakeRoom(root_, limits_, text.size(), "", owned ? path : "");
    if (--pins[guard] == 0)
        pins.erase(guard);
    if (!room)
        return false;
    const int fd = open((path + ".part").c_str(), O_WRONLY | O_CREAT | O_EXCL, 0664);
    if (fd < 0)
        return false;
    FILE* file = fdopen(fd, "wb");
    if (!file) {
        close(fd);
        std::remove((path + ".part").c_str());
        return false;
    }
    bool ok = std::fwrite(text.data(), 1, text.size(), file) == text.size();
    ok = std::fclose(file) == 0 && ok;
    const int meta_fd = open((path + ".meta.part").c_str(), O_WRONLY | O_CREAT | O_EXCL, 0664);
    FILE* marker = meta_fd < 0 ? nullptr : fdopen(meta_fd, "wb");
    if (marker) {
        ok = std::fputs(kLyricMarker, marker) >= 0 &&
             std::fprintf(marker, "%u %u\n", unsigned(text.size()),
                          unsigned(Checksum(2166136261U, text.data(), text.size()))) > 0 &&
             ok;
        ok = std::fclose(marker) == 0 && ok;
    } else {
        if (meta_fd >= 0)
            close(meta_fd);
        ok = false;
    }
    if (ok) {
        ok = PublishOwnedFiles({{path + ".meta.part", path + ".meta"}, {path + ".part", path}});
    }
    std::remove((path + ".part").c_str());
    if (meta_fd >= 0)
        std::remove((path + ".meta.part").c_str());
    return ok;
}
