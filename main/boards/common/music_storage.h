#ifndef MUSIC_STORAGE_H_
#define MUSIC_STORAGE_H_

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

struct MusicFile {
    std::string path;
    std::string title;
};

class MusicStorage {
public:
    virtual ~MusicStorage() = default;
    virtual bool IsReady() const = 0;
    virtual std::vector<MusicFile> ListChildrenMusic(std::string& error) = 0;
    virtual std::optional<std::string> FindCachedPath(const std::string& cache_key) = 0;
    virtual std::FILE* OpenRead(const std::string& path) = 0;
    virtual std::FILE* OpenCacheWrite(const std::string& cache_key, size_t expected_bytes,
                                      std::string& temporary_path) = 0;
    virtual bool CommitCache(const std::string& temporary_path,
                             const std::string& cache_key) = 0;
    virtual void DiscardCache(const std::string& temporary_path) = 0;
};

#endif  // MUSIC_STORAGE_H_
