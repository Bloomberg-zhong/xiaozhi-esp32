#ifndef MUSIC_LRC_PARSER_H_
#define MUSIC_LRC_PARSER_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct LyricLine {
    uint32_t time_ms = 0;
    std::string text;
};

// Parses LRC text into lines sorted by time. Supports multiple time tags per
// line ("[00:01.00][00:30.00]text"), "mm:ss", "mm:ss.xx", "mm:ss.xxx" and
// "mm:ss:xx" tags, and the global "[offset:+/-ms]" tag. Metadata tags such as
// "[ar:...]" are ignored. At most `max_lines` lines are kept.
std::vector<LyricLine> ParseLrc(const std::string& lrc, size_t max_lines = 512);

// Returns the index of the line active at `position_ms`, or -1 before the
// first line.
int FindLyricIndex(const std::vector<LyricLine>& lines, uint32_t position_ms);

#endif  // MUSIC_LRC_PARSER_H_
