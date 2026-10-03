#include "local_music.h"
#include "music_cache.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#ifdef ESP_PLATFORM
#include <esp_log.h>
#endif

namespace {

std::string ToLowerAscii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool IsSupportedExtension(const std::string& name) {
    size_t dot = name.rfind('.');
    if (dot == std::string::npos) {
        return false;
    }
    const std::string extension = ToLowerAscii(name.substr(dot + 1));
    return extension == "mp3" || extension == "m4a" || extension == "aac" || extension == "flac" ||
           extension == "wav";
}

bool IsExcluded(const std::string& name, const LocalMusicScanOptions& options) {
    const std::string lower = ToLowerAscii(name);
    for (const auto& excluded : options.excluded_folders) {
        if (lower == ToLowerAscii(excluded)) {
            return true;
        }
    }
    return false;
}

bool FileExists(const std::string& path) {
    struct stat info;
    return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

MusicTrack MakeTrack(const std::string& folder, const std::string& name) {
    MusicTrack track;
    track.stream_url = folder + "/" + name;
    track.id = track.stream_url;

    std::string stem = name.substr(0, name.rfind('.'));
    size_t separator = stem.find(" - ");
    if (separator != std::string::npos && separator > 0 && separator + 3 < stem.size()) {
        track.artist = stem.substr(0, separator);
        track.title = stem.substr(separator + 3);
    } else {
        track.title = stem;
    }
    size_t slash = folder.rfind('/');
    track.album = slash == std::string::npos ? folder : folder.substr(slash + 1);

    std::string lyric = folder + "/" + stem + ".lrc";
    if (FileExists(lyric)) {
        track.lyric_url = lyric;
    }
    return track;
}

void Scan(const std::string& folder, int depth, const LocalMusicScanOptions& options,
          std::vector<MusicTrack>& tracks) {
#ifdef ESP_PLATFORM
    ESP_LOGI("LocalMusic", "Scanning folder: %s", folder.c_str());
#endif
    DIR* dir = opendir(folder.c_str());
    if (dir == nullptr) {
        return;
    }
    std::vector<std::string> files;
    std::vector<std::string> folders;
    while (struct dirent* entry = readdir(dir)) {
        std::string name = entry->d_name;
        if (name.empty() || name[0] == '.') {
            continue;
        }
        bool is_dir = entry->d_type == DT_DIR;
        bool is_file = entry->d_type == DT_REG;
        if (!is_dir && !is_file) {
            struct stat info;
            std::string path = folder + "/" + name;
            if (stat(path.c_str(), &info) != 0) {
                continue;
            }
            is_dir = S_ISDIR(info.st_mode);
            is_file = S_ISREG(info.st_mode);
        }
        if (is_dir) {
            if (depth < options.max_depth && !IsExcluded(name, options)) {
                folders.push_back(std::move(name));
            }
        } else if (is_file && IsSupportedExtension(name)) {
            files.push_back(std::move(name));
        }
    }
    closedir(dir);
#ifdef ESP_PLATFORM
    ESP_LOGI("LocalMusic", "Listed %s: %u audio files, %u folders", folder.c_str(),
             unsigned(files.size()), unsigned(folders.size()));
#endif

    std::sort(files.begin(), files.end());
    std::sort(folders.begin(), folders.end());
    for (const auto& name : files) {
        if (tracks.size() >= options.max_tracks) {
            return;
        }
        MusicTrack track = MakeTrack(folder, name);
#ifdef ESP_PLATFORM
        ESP_LOGI("LocalMusic", "Inspecting song: %s", track.stream_url.c_str());
#endif
        if (MusicCache::IsManagedPath(track.stream_url) &&
            !MusicCache::ReadTrack(track.stream_url, track, options.verify_cache_audio)) {
            continue;
        }
        tracks.push_back(std::move(track));
    }
    for (const auto& name : folders) {
        if (tracks.size() >= options.max_tracks) {
            return;
        }
        Scan(folder + "/" + name, depth + 1, options, tracks);
    }
}

}  // namespace

bool IsLocalMusicPath(const std::string& path) { return !path.empty() && path.front() == '/'; }

std::vector<MusicTrack> ScanLocalMusic(const std::string& folder,
                                       const LocalMusicScanOptions& options) {
    std::vector<MusicTrack> tracks;
    std::string root = folder;
    while (root.size() > 1 && root.back() == '/') {
        root.pop_back();
    }
    if (!root.empty()) {
        Scan(root, 0, options, tracks);
    }
    return tracks;
}

std::vector<MusicTrack> FilterLocalMusic(const std::vector<MusicTrack>& tracks,
                                         const std::string& query) {
    std::vector<std::string> words;
    size_t start = 0;
    const std::string lower_query = ToLowerAscii(query);
    while (start < lower_query.size()) {
        size_t end = lower_query.find_first_of(" \t", start);
        if (end == std::string::npos) {
            end = lower_query.size();
        }
        if (end > start) {
            words.push_back(lower_query.substr(start, end - start));
        }
        start = end + 1;
    }
    if (words.empty()) {
        return tracks;
    }

    std::vector<MusicTrack> matches;
    for (const auto& track : tracks) {
        const std::string haystack = ToLowerAscii(track.title + " " + track.artist + " " +
                                                  track.album + " " + track.stream_url);
        if (std::all_of(words.begin(), words.end(), [&haystack](const std::string& word) {
                return haystack.find(word) != std::string::npos;
            })) {
            matches.push_back(track);
        }
    }
    return matches;
}

bool ReadLocalTextFile(const std::string& path, size_t max_bytes, std::string& text) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return false;
    }
    text.clear();
    char buffer[512];
    bool ok = true;
    while (true) {
        size_t size = std::fread(buffer, 1, sizeof(buffer), file);
        if (size == 0) {
            ok = !std::ferror(file);
            break;
        }
        if (text.size() + size > max_bytes) {
            ok = false;
            break;
        }
        text.append(buffer, size);
    }
    std::fclose(file);
    return ok;
}
