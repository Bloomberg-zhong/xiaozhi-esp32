#include "lrc_parser.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace {

std::string Trim(const std::string& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return text.substr(begin, end - begin);
}

bool ParseDigits(const std::string& text, size_t begin, size_t end, int& value) {
    if (begin >= end || end - begin > 6) {
        return false;
    }
    value = 0;
    for (size_t i = begin; i < end; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(text[i]))) {
            return false;
        }
        value = value * 10 + (text[i] - '0');
    }
    return true;
}

// Parses "mm:ss", "mm:ss.xx", "mm:ss.xxx" or "mm:ss:xx".
bool ParseTimeTag(const std::string& tag, int64_t& time_ms) {
    size_t colon = tag.find(':');
    if (colon == std::string::npos) {
        return false;
    }
    int minutes = 0;
    if (!ParseDigits(tag, 0, colon, minutes)) {
        return false;
    }
    size_t separator = tag.find_first_of(".:", colon + 1);
    size_t seconds_end = separator == std::string::npos ? tag.size() : separator;
    int seconds = 0;
    if (!ParseDigits(tag, colon + 1, seconds_end, seconds) || seconds > 59) {
        return false;
    }
    int fraction_ms = 0;
    if (separator != std::string::npos) {
        int fraction = 0;
        size_t digits = tag.size() - separator - 1;
        if (digits == 0 || digits > 3 || !ParseDigits(tag, separator + 1, tag.size(), fraction)) {
            return false;
        }
        fraction_ms = digits == 1 ? fraction * 100 : digits == 2 ? fraction * 10 : fraction;
    }
    time_ms = static_cast<int64_t>(minutes) * 60000 + seconds * 1000 + fraction_ms;
    return true;
}

bool ParseOffsetTag(const std::string& tag, int64_t& offset_ms) {
    static const char kPrefix[] = "offset:";
    if (tag.compare(0, sizeof(kPrefix) - 1, kPrefix) != 0) {
        return false;
    }
    std::string value = Trim(tag.substr(sizeof(kPrefix) - 1));
    if (value.empty()) {
        return false;
    }
    char* end = nullptr;
    long parsed = std::strtol(value.c_str(), &end, 10);
    if (end == nullptr || *end != '\0') {
        return false;
    }
    offset_ms = parsed;
    return true;
}

}  // namespace

std::vector<LyricLine> ParseLrc(const std::string& lrc, size_t max_lines) {
    struct TimedLine {
        int64_t time_ms;
        std::string text;
    };
    std::vector<TimedLine> timed;
    int64_t offset_ms = 0;

    size_t line_start = 0;
    while (line_start < lrc.size() && timed.size() < max_lines) {
        size_t line_end = lrc.find('\n', line_start);
        if (line_end == std::string::npos) {
            line_end = lrc.size();
        }
        std::string line = lrc.substr(line_start, line_end - line_start);
        line_start = line_end + 1;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        std::vector<int64_t> times;
        size_t cursor = 0;
        while (cursor < line.size() && line[cursor] == '[') {
            size_t close = line.find(']', cursor + 1);
            if (close == std::string::npos) {
                break;
            }
            std::string tag = line.substr(cursor + 1, close - cursor - 1);
            int64_t time_ms = 0;
            if (ParseTimeTag(tag, time_ms)) {
                times.push_back(time_ms);
            } else {
                ParseOffsetTag(tag, offset_ms);
            }
            cursor = close + 1;
        }
        if (times.empty()) {
            continue;
        }
        std::string text = Trim(line.substr(cursor));
        for (int64_t time_ms : times) {
            if (timed.size() >= max_lines) {
                break;
            }
            timed.push_back({time_ms, text});
        }
    }

    std::stable_sort(timed.begin(), timed.end(),
                     [](const TimedLine& a, const TimedLine& b) { return a.time_ms < b.time_ms; });

    std::vector<LyricLine> lines;
    lines.reserve(timed.size());
    for (auto& item : timed) {
        // A positive offset shows lyrics earlier, as defined by the LRC format.
        int64_t time_ms = item.time_ms - offset_ms;
        lines.push_back(
            {static_cast<uint32_t>(std::max<int64_t>(time_ms, 0)), std::move(item.text)});
    }
    return lines;
}

int FindLyricIndex(const std::vector<LyricLine>& lines, uint32_t position_ms) {
    auto it = std::upper_bound(
        lines.begin(), lines.end(), position_ms,
        [](uint32_t position, const LyricLine& line) { return position < line.time_ms; });
    return static_cast<int>(it - lines.begin()) - 1;
}
