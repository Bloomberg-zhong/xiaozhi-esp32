#ifndef MUSIC_LOCAL_MUSIC_H_
#define MUSIC_LOCAL_MUSIC_H_

// Local music library helpers (SD card or any mounted VFS path). They only use
// POSIX file APIs, so they are unit tested on the host.

#include <cstddef>
#include <string>
#include <vector>

#include "music_source.h"

// Folder for pomodoro white noise, relative to the local music root. It is
// excluded from the regular music library.
constexpr const char* kWhiteNoiseFolder = "white-noise";

struct LocalMusicScanOptions {
    size_t max_tracks = 500;
    int max_depth = 4;
    // Fast listings defer the managed audio checksum to the selected track's reader.
    bool verify_cache_audio = true;
    // Folder names (case-insensitive) skipped at any depth.
    std::vector<std::string> excluded_folders;
};

// True for absolute file system paths such as "/sdcard/a.mp3".
bool IsLocalMusicPath(const std::string& path);

// Recursively lists supported audio files (.mp3 .m4a .aac .flac .wav) under
// `folder`, sorted by path. Hidden entries are skipped. "Artist - Title.mp3"
// names fill in the artist; the parent folder becomes the album; a sibling
// ".lrc" file becomes the lyric path.
std::vector<MusicTrack> ScanLocalMusic(const std::string& folder,
                                       const LocalMusicScanOptions& options = {});

// Keeps the tracks whose title, artist, album or path contains every
// whitespace-separated keyword of `query` (ASCII case-insensitive).
std::vector<MusicTrack> FilterLocalMusic(const std::vector<MusicTrack>& tracks,
                                         const std::string& query);

// Reads at most `max_bytes` of a text file.
bool ReadLocalTextFile(const std::string& path, size_t max_bytes, std::string& text);

#endif  // MUSIC_LOCAL_MUSIC_H_
