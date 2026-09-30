#include "favorites.h"

#include <algorithm>

namespace {

constexpr size_t kMaxIdBytes = 200;
constexpr size_t kMaxTitleBytes = 90;
constexpr size_t kMaxArtistBytes = 60;

bool IsValidKind(char kind) { return kind == 's' || kind == 'r' || kind == 'l'; }

// Tabs and line breaks delimit the format; a cut UTF-8 sequence is repaired by
// dropping its leading bytes.
std::string Clean(const std::string& text, size_t max_bytes) {
    std::string cleaned;
    cleaned.reserve(std::min(text.size(), max_bytes));
    for (char c : text) {
        cleaned.push_back(c == '\t' || c == '\n' || c == '\r' ? ' ' : c);
    }
    if (cleaned.size() > max_bytes) {
        size_t cut = max_bytes;
        while (cut > 0 && (static_cast<unsigned char>(cleaned[cut]) & 0xC0) == 0x80) {
            --cut;
        }
        cleaned.resize(cut);
    }
    return cleaned;
}

}  // namespace

size_t FavoriteList::EntryBytes(const FavoriteEntry& entry) {
    return 1 + 1 + entry.id.size() + 1 + entry.title.size() + 1 + entry.artist.size() + 1;
}

bool FavoriteList::Add(FavoriteEntry entry) {
    if (!IsValidKind(entry.kind) || entry.id.empty() || entry.id.size() > kMaxIdBytes) {
        return false;
    }
    entry.title = Clean(entry.title, kMaxTitleBytes);
    entry.artist = Clean(entry.artist, kMaxArtistBytes);
    entry.id = Clean(entry.id, kMaxIdBytes);
    if (EntryBytes(entry) > kMaxBytes) {
        return false;
    }

    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [&entry](const FavoriteEntry& item) {
                                    return item.kind == entry.kind && item.id == entry.id;
                                }),
                 items_.end());
    items_.insert(items_.begin(), std::move(entry));

    while (items_.size() > kMaxEntries) {
        items_.pop_back();
    }
    size_t bytes = 0;
    for (const auto& item : items_) {
        bytes += EntryBytes(item);
    }
    while (bytes > kMaxBytes && items_.size() > 1) {
        bytes -= EntryBytes(items_.back());
        items_.pop_back();
    }
    return true;
}

bool FavoriteList::Remove(size_t index) {
    if (index >= items_.size()) {
        return false;
    }
    items_.erase(items_.begin() + index);
    return true;
}

bool FavoriteList::Contains(char kind, const std::string& id) const {
    return std::any_of(items_.begin(), items_.end(), [&](const FavoriteEntry& item) {
        return item.kind == kind && item.id == id;
    });
}

std::string FavoriteList::Serialize() const {
    std::string text;
    for (const auto& item : items_) {
        text.push_back(item.kind);
        text.push_back('\t');
        text += item.id;
        text.push_back('\t');
        text += item.title;
        text.push_back('\t');
        text += item.artist;
        text.push_back('\n');
    }
    return text;
}

FavoriteList FavoriteList::Parse(const std::string& text) {
    FavoriteList list;
    std::vector<FavoriteEntry> parsed;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = text.substr(start, end - start);
        start = end + 1;

        // kind \t id \t title \t artist
        size_t first = line.find('\t');
        size_t second = first == std::string::npos ? first : line.find('\t', first + 1);
        size_t third = second == std::string::npos ? second : line.find('\t', second + 1);
        if (third == std::string::npos || first != 1) {
            continue;
        }
        FavoriteEntry entry;
        entry.kind = line[0];
        entry.id = line.substr(first + 1, second - first - 1);
        entry.title = line.substr(second + 1, third - second - 1);
        entry.artist = line.substr(third + 1);
        if (!IsValidKind(entry.kind) || entry.id.empty() || entry.id.size() > kMaxIdBytes) {
            continue;
        }
        parsed.push_back(std::move(entry));
    }
    // Stored newest first; re-adding oldest first restores the order and
    // enforces the current limits.
    for (auto it = parsed.rbegin(); it != parsed.rend(); ++it) {
        list.Add(*it);
    }
    return list;
}
