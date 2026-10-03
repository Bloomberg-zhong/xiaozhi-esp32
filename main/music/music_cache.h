#ifndef MUSIC_CACHE_H_
#define MUSIC_CACHE_H_

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "music_source.h"

// SD cache owns only files carrying its metadata marker. All I/O runs on the
// music reader task; cache failures never prevent normal streamed playback.
class MusicCache {
public:
    struct Limits {
        uint64_t max_bytes = 256ULL * 1024 * 1024;
        uint64_t min_free_bytes = 32ULL * 1024 * 1024;
    };
    struct Entry {
        MusicTrack track;
        ~Entry();
        std::string guard;
        bool artwork_only = false;
    };
    class Writer {
    public:
        ~Writer();
        bool Append(size_t offset, const char* bytes, size_t size);
        bool Finish();
        // Persist a bounded continuation job without signed URLs/credentials.
        bool Suspend();
        size_t offset() const { return written_; }
        // Writer ownership keeps this validated prefix stable for foreground replay.
        const std::string& PartialPath() const { return part_; }
        void Abort();

    private:
        friend class MusicCache;
        FILE* file_ = nullptr;
        std::string root_, stem_, guard_, part_, base_;
        MusicTrack track_;
        uint64_t expected_ = 0, written_ = 0;
        uint32_t checksum_ = 2166136261U;
        uint8_t header_[12] = {};
        size_t header_size_ = 0;
        uint64_t min_free_bytes_ = 0;
    };

    explicit MusicCache(std::string root);
    MusicCache(std::string root, Limits limits);
    std::shared_ptr<Entry> Find(const MusicTrack& track, const std::string& source_base) const;
    std::shared_ptr<Entry> PinLocal(const std::string& audio_path) const;
    std::shared_ptr<Entry> PinArtwork(const MusicTrack& track,
                                      const std::string& source_base) const;
    bool IsWriting(const MusicTrack& track, const std::string& source_base) const;
    static bool SameIdentity(const MusicTrack& left, const std::string& left_base,
                             const MusicTrack& right, const std::string& right_base);
    std::unique_ptr<Writer> Begin(const MusicTrack& track, const std::string& source_base,
                                  uint64_t expected_bytes) const;
    std::string CoverPath(const MusicTrack& track, const std::string& source_base) const;
    // Recover the online artwork URL from owned local audio metadata without
    // reading or checksumming the audio payload.
    std::string OriginalCoverUrl(const MusicTrack& track) const;
    // JPEG bytes have already been validated by the cover loader; at most64KiB.
    bool StoreCover(const MusicTrack& track, const std::string& source_base, const char* data,
                    size_t size) const;
    // Preserve fetched synchronized lyrics independently of the audio writer's
    // earlier track snapshot. Managed companions contain at most 64KiB/512 lines.
    bool StoreLyrics(const MusicTrack& track, const std::string& source_base,
                     const std::vector<LyricLine>& lines) const;
    bool ReadLyrics(const MusicTrack& track, const std::string& source_base,
                    std::string& text) const;
    std::vector<MusicTrack> Pending(const std::string& source_base) const;
    // Restore the original title/artist and reject missing/truncated managed audio.
    // Fast listings may defer the checksum; PinLocal still verifies before playback.
    static bool IsManagedPath(const std::string& audio_path);
    static bool ReadTrack(const std::string& audio_path, MusicTrack& track,
                          bool verify_audio = true);

private:
    std::string root_;
    Limits limits_;
};

#endif  // MUSIC_CACHE_H_
