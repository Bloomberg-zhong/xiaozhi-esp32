#include "music_storage.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <driver/sdmmc_host.h>
#include <sdmmc_cmd.h>

#include "config.h"

namespace {

constexpr char kMountPoint[] = "/sdcard";
constexpr char kChildrenDirectory[] = "/sdcard/儿童音乐";
constexpr char kCacheDirectory[] = "/sdcard/.xiaozhi/cache";
constexpr uint64_t kCacheLimitBytes = 512ULL * 1024 * 1024;
constexpr uint64_t kFreeSpaceReserveBytes = 8ULL * 1024 * 1024;
constexpr size_t kMaxMusicFiles = 256;
constexpr unsigned kMaxDirectoryDepth = 4;
constexpr char kTag[] = "RlcdMusicStorage";

std::string CacheName(const std::string& key) {
    uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : key) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    char name[24];
    std::snprintf(name, sizeof(name), "%016llx.mp3", static_cast<unsigned long long>(hash));
    return name;
}

bool IsMp3(const char* name) {
    const char* dot = std::strrchr(name, '.');
    return dot != nullptr && std::strlen(dot) == 4 &&
           std::tolower(static_cast<unsigned char>(dot[1])) == 'm' &&
           std::tolower(static_cast<unsigned char>(dot[2])) == 'p' &&
           std::tolower(static_cast<unsigned char>(dot[3])) == '3';
}

struct CacheEntry {
    std::string path;
    uint64_t bytes;
    time_t modified;
};

void CollectMp3Files(const std::string& directory, unsigned depth, std::vector<MusicFile>& files) {
    if (depth > kMaxDirectoryDepth || files.size() >= kMaxMusicFiles) {
        return;
    }
    DIR* dir = opendir(directory.c_str());
    if (dir == nullptr) {
        return;
    }
    while (files.size() < kMaxMusicFiles) {
        dirent* entry = readdir(dir);
        if (entry == nullptr) {
            break;
        }
        if (entry->d_name[0] == '.' || std::strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        const std::string path = directory + "/" + entry->d_name;
        struct stat info = {};
        if (stat(path.c_str(), &info) != 0) {
            continue;
        }
        if (S_ISDIR(info.st_mode)) {
            CollectMp3Files(path, depth + 1, files);
        } else if (S_ISREG(info.st_mode) && IsMp3(entry->d_name)) {
            std::string title(entry->d_name);
            const size_t extension = title.find_last_of('.');
            if (extension != std::string::npos) {
                title.resize(extension);
            }
            files.push_back({path, std::move(title)});
        }
    }
    closedir(dir);
}

}  // namespace

class RlcdMusicStorage final : public MusicStorage {
public:
    RlcdMusicStorage() {
        std::lock_guard<std::mutex> lock(mutex_);
        MountCard();
    }

    bool IsReady() const override { return ready_.load(); }

    std::vector<MusicFile> ListChildrenMusic(std::string& error) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<MusicFile> files;
        if (!MountCard()) {
            error = "未检测到可用的 FAT32 TF 卡";
            return files;
        }
        struct stat info = {};
        if (stat(kChildrenDirectory, &info) != 0 || !S_ISDIR(info.st_mode)) {
            error = "TF 卡中没有 儿童音乐 文件夹，请在卡根目录创建该文件夹";
            return files;
        }
        CollectMp3Files(kChildrenDirectory, 0, files);
        std::sort(files.begin(), files.end(), [](const MusicFile& left, const MusicFile& right) {
            return left.path < right.path;
        });
        if (files.empty()) {
            error = "儿童音乐 文件夹里还没有 MP3 歌曲";
        }
        return files;
    }

    std::optional<std::string> FindCachedPath(const std::string& cache_key) override {
        if (cache_key.empty()) return std::nullopt;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!MountCard()) return std::nullopt;
        const std::string path = std::string(kCacheDirectory) + "/" + CacheName(cache_key);
        struct stat info = {};
        if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_size <= 0) {
            return std::nullopt;
        }
        utime(path.c_str(), nullptr);
        return path;
    }

    std::FILE* OpenRead(const std::string& path) override {
        if (path.compare(0, std::strlen(kMountPoint), kMountPoint) != 0) return nullptr;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!MountCard()) return nullptr;
        return std::fopen(path.c_str(), "rb");
    }

    std::FILE* OpenCacheWrite(const std::string& cache_key, size_t expected_bytes,
                              std::string& temporary_path) override {
        if (cache_key.empty() || expected_bytes > kCacheLimitBytes) return nullptr;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!MountCard() || !EnsureSpace(expected_bytes)) return nullptr;
        temporary_path = std::string(kCacheDirectory) + "/" + CacheName(cache_key) + ".part";
        std::remove(temporary_path.c_str());
        return std::fopen(temporary_path.c_str(), "wb");
    }

    bool CommitCache(const std::string& temporary_path,
                     const std::string& cache_key) override {
        if (cache_key.empty()) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!MountCard()) return false;
        const std::string expected_prefix = std::string(kCacheDirectory) + "/";
        if (temporary_path.compare(0, expected_prefix.size(), expected_prefix) != 0 ||
            temporary_path.size() < 5 || temporary_path.compare(temporary_path.size() - 5, 5,
                                                                  ".part") != 0) {
            return false;
        }
        const std::string final_path = expected_prefix + CacheName(cache_key);
        std::remove(final_path.c_str());
        return std::rename(temporary_path.c_str(), final_path.c_str()) == 0;
    }

    void DiscardCache(const std::string& temporary_path) override {
        if (temporary_path.compare(0, std::strlen(kCacheDirectory), kCacheDirectory) == 0) {
            std::remove(temporary_path.c_str());
        }
    }

private:
    bool MountCard() {
        if (ready_.load()) return true;
        esp_err_t error = ESP_OK;
        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
        slot.width = 1;
        slot.clk = RLCD_SD_CLK_PIN;
        slot.cmd = RLCD_SD_CMD_PIN;
        slot.d0 = RLCD_SD_D0_PIN;
        esp_vfs_fat_sdmmc_mount_config_t mount = {};
        mount.format_if_mount_failed = false;
        mount.max_files = 8;
        mount.allocation_unit_size = 16 * 1024;
        error = esp_vfs_fat_sdmmc_mount(kMountPoint, &host, &slot, &mount, &card_);
        if (error != ESP_OK) {
            ESP_LOGW(kTag, "No usable FAT32 TF card: %s", esp_err_to_name(error));
            card_ = nullptr;
            return false;
        }
        ready_.store(true);
        mkdir(kChildrenDirectory, 0775);
        mkdir("/sdcard/.xiaozhi", 0775);
        mkdir(kCacheDirectory, 0775);
        RemovePartialFiles();
        ESP_LOGI(kTag, "TF card mounted at %s", kMountPoint);
        return true;
    }
    bool EnsureSpace(uint64_t expected_bytes) {
        const uint64_t required = std::min<uint64_t>(expected_bytes, kCacheLimitBytes) +
                                  kFreeSpaceReserveBytes;
        std::vector<CacheEntry> entries;
        DIR* dir = opendir(kCacheDirectory);
        if (dir == nullptr) {
            return false;
        }
        uint64_t cache_bytes = 0;
        while (dirent* entry = readdir(dir)) {
            if (entry->d_name[0] == '.' || !IsMp3(entry->d_name)) {
                continue;
            }
            const std::string path = std::string(kCacheDirectory) + "/" + entry->d_name;
            struct stat info = {};
            if (stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode)) {
                entries.push_back({path, static_cast<uint64_t>(info.st_size), info.st_mtime});
                cache_bytes += static_cast<uint64_t>(info.st_size);
            }
        }
        closedir(dir);
        std::sort(entries.begin(), entries.end(), [](const CacheEntry& left,
                                                     const CacheEntry& right) {
            return left.modified < right.modified;
        });
        uint64_t total_bytes = 0;
        uint64_t free_bytes = 0;
        if (esp_vfs_fat_info(kMountPoint, &total_bytes, &free_bytes) != ESP_OK) {
            return false;
        }
        const uint64_t target_cache = kCacheLimitBytes -
                                      std::min<uint64_t>(expected_bytes, kCacheLimitBytes);
        for (const auto& entry : entries) {
            if (cache_bytes <= target_cache && free_bytes >= required) {
                break;
            }
            if (std::remove(entry.path.c_str()) == 0) {
                cache_bytes -= entry.bytes;
                free_bytes += entry.bytes;
            }
        }
        return cache_bytes <= target_cache && free_bytes >= required;
    }

    void RemovePartialFiles() {
        DIR* dir = opendir(kCacheDirectory);
        if (dir == nullptr) {
            return;
        }
        while (dirent* entry = readdir(dir)) {
            const size_t name_length = std::strlen(entry->d_name);
            if (name_length > 5 && std::strcmp(entry->d_name + name_length - 5, ".part") == 0) {
                const std::string path = std::string(kCacheDirectory) + "/" + entry->d_name;
                std::remove(path.c_str());
            }
        }
        closedir(dir);
    }

    mutable std::mutex mutex_;
    sdmmc_card_t* card_ = nullptr;
    std::atomic<bool> ready_{false};
};

MusicStorage* CreateRlcdMusicStorage() { return new RlcdMusicStorage(); }
