#ifndef MUSIC_FAVORITES_H_
#define MUSIC_FAVORITES_H_

// The user's favorite songs. Plain C++ so the format and limits are unit
// tested on the host. The firmware stores the serialized text in NVS, which
// only has 16 KB in total, hence the small byte budget.

#include <cstddef>
#include <string>
#include <vector>

struct FavoriteEntry {
    // 's': a song of the configured music source, by `id`;
    // 'r': a live stream (radio station) of the music source;
    // 'l': a file on local storage, `id` is its path.
    char kind = 's';
    std::string id;
    std::string title;
    std::string artist;
};

class FavoriteList {
public:
    static constexpr size_t kMaxEntries = 30;
    // NVS strings are limited to 4000 bytes.
    static constexpr size_t kMaxBytes = 3600;

    // Newest first. An entry with the same kind and id is moved to the front.
    // The oldest entries are dropped to respect kMaxEntries and kMaxBytes.
    // Returns false when `entry` itself cannot be stored.
    bool Add(FavoriteEntry entry);
    bool Remove(size_t index);
    bool Contains(char kind, const std::string& id) const;
    void Clear() { items_.clear(); }

    const std::vector<FavoriteEntry>& items() const { return items_; }
    size_t size() const { return items_.size(); }

    // One entry per line: kind TAB id TAB title TAB artist.
    std::string Serialize() const;
    static FavoriteList Parse(const std::string& text);

private:
    std::vector<FavoriteEntry> items_;
    static size_t EntryBytes(const FavoriteEntry& entry);
};

#endif  // MUSIC_FAVORITES_H_
