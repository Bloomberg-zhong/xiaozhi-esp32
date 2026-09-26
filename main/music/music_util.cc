#include "music_util.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace {

std::string ToLower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool StartsWith(const std::string& text, const char* prefix) {
    return text.compare(0, std::strlen(prefix), prefix) == 0;
}

MusicAudioFormat FormatFromMagic(const uint8_t* data, size_t size) {
    if (size >= 3 && std::memcmp(data, "ID3", 3) == 0) {
        return MusicAudioFormat::kMp3;
    }
    if (size >= 4 && std::memcmp(data, "fLaC", 4) == 0) {
        return MusicAudioFormat::kFlac;
    }
    if (size >= 12 && std::memcmp(data, "RIFF", 4) == 0 && std::memcmp(data + 8, "WAVE", 4) == 0) {
        return MusicAudioFormat::kWav;
    }
    if (size >= 8 && std::memcmp(data + 4, "ftyp", 4) == 0) {
        return MusicAudioFormat::kM4a;
    }
    if (size >= 2 && data[0] == 0xFF && (data[1] & 0xE0) == 0xE0) {
        // MPEG audio frame sync. Layer bits "00" mean ADTS AAC.
        const int layer = (data[1] >> 1) & 0x03;
        return layer == 0 ? MusicAudioFormat::kAac : MusicAudioFormat::kMp3;
    }
    return MusicAudioFormat::kUnknown;
}

MusicAudioFormat FormatFromContentType(const std::string& content_type) {
    std::string type = ToLower(content_type);
    size_t semicolon = type.find(';');
    if (semicolon != std::string::npos) {
        type.resize(semicolon);
    }
    while (!type.empty() && std::isspace(static_cast<unsigned char>(type.back()))) {
        type.pop_back();
    }
    if (type == "audio/mpeg" || type == "audio/mp3" || type == "audio/mpeg3" ||
        type == "audio/x-mpeg" || type == "audio/x-mp3") {
        return MusicAudioFormat::kMp3;
    }
    if (type == "audio/aac" || type == "audio/aacp" || type == "audio/x-aac") {
        return MusicAudioFormat::kAac;
    }
    if (type == "audio/mp4" || type == "audio/x-m4a" || type == "audio/m4a") {
        return MusicAudioFormat::kM4a;
    }
    if (type == "audio/flac" || type == "audio/x-flac") {
        return MusicAudioFormat::kFlac;
    }
    if (type == "audio/wav" || type == "audio/x-wav" || type == "audio/wave" ||
        type == "audio/vnd.wave") {
        return MusicAudioFormat::kWav;
    }
    return MusicAudioFormat::kUnknown;
}

MusicAudioFormat FormatFromUrl(const std::string& url) {
    std::string path = url;
    size_t end = path.find_first_of("?#");
    if (end != std::string::npos) {
        path.resize(end);
    }
    size_t dot = path.rfind('.');
    size_t slash = path.rfind('/');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
        return MusicAudioFormat::kUnknown;
    }
    std::string extension = ToLower(path.substr(dot + 1));
    if (extension == "mp3") {
        return MusicAudioFormat::kMp3;
    }
    if (extension == "aac") {
        return MusicAudioFormat::kAac;
    }
    if (extension == "m4a" || extension == "mp4") {
        return MusicAudioFormat::kM4a;
    }
    if (extension == "flac") {
        return MusicAudioFormat::kFlac;
    }
    if (extension == "wav") {
        return MusicAudioFormat::kWav;
    }
    return MusicAudioFormat::kUnknown;
}

}  // namespace

const char* MusicAudioFormatName(MusicAudioFormat format) {
    switch (format) {
        case MusicAudioFormat::kMp3:
            return "mp3";
        case MusicAudioFormat::kAac:
            return "aac";
        case MusicAudioFormat::kM4a:
            return "m4a";
        case MusicAudioFormat::kFlac:
            return "flac";
        case MusicAudioFormat::kWav:
            return "wav";
        default:
            return "unknown";
    }
}

MusicAudioFormat DetectMusicAudioFormat(const uint8_t* data, size_t size,
                                        const std::string& content_type, const std::string& url) {
    MusicAudioFormat format = FormatFromMagic(data, size);
    if (format == MusicAudioFormat::kUnknown) {
        format = FormatFromContentType(content_type);
    }
    if (format == MusicAudioFormat::kUnknown) {
        format = FormatFromUrl(url);
    }
    return format;
}

bool IsHttpUrl(const std::string& url) {
    std::string lower = ToLower(url.substr(0, 8));
    return (StartsWith(lower, "http://") && url.size() > 7) ||
           (StartsWith(lower, "https://") && url.size() > 8);
}

std::string UrlEncode(const std::string& value) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size() * 3);
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded.push_back(static_cast<char>(c));
        } else {
            encoded.push_back('%');
            encoded.push_back(kHex[c >> 4]);
            encoded.push_back(kHex[c & 0x0F]);
        }
    }
    return encoded;
}

std::string BuildUrl(const std::string& base, const std::string& path,
                     const std::vector<std::pair<std::string, std::string>>& query) {
    std::string url = base;
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    if (!path.empty()) {
        if (path.front() != '/') {
            url.push_back('/');
        }
        url += path;
    }
    char separator = url.find('?') == std::string::npos ? '?' : '&';
    for (const auto& [key, value] : query) {
        url.push_back(separator);
        url += UrlEncode(key);
        url.push_back('=');
        url += UrlEncode(value);
        separator = '&';
    }
    return url;
}

std::string UrlOrigin(const std::string& url) {
    if (!IsHttpUrl(url)) {
        return "";
    }
    size_t host_start = url.find("://") + 3;
    size_t host_end = url.find_first_of("/?#", host_start);
    return host_end == std::string::npos ? url : url.substr(0, host_end);
}

std::string ResolveUrl(const std::string& base, const std::string& reference) {
    if (reference.empty() || IsHttpUrl(reference)) {
        return reference;
    }
    if (reference.front() == '/') {
        return UrlOrigin(base) + reference;
    }
    std::string prefix = base;
    size_t query = prefix.find_first_of("?#");
    if (query != std::string::npos) {
        prefix.resize(query);
    }
    if (prefix.empty() || prefix.back() != '/') {
        prefix.push_back('/');
    }
    return prefix + reference;
}

size_t DownmixToMono16(const uint8_t* data, size_t bytes, int channels, int bits_per_sample,
                       std::vector<int16_t>& out) {
    if (channels <= 0 ||
        (bits_per_sample != 16 && bits_per_sample != 24 && bits_per_sample != 32)) {
        return 0;
    }
    const size_t sample_bytes = bits_per_sample / 8;
    const size_t frame_bytes = sample_bytes * channels;
    const size_t frames = bytes / frame_bytes;
    out.reserve(out.size() + frames);
    for (size_t frame = 0; frame < frames; ++frame) {
        const uint8_t* p = data + frame * frame_bytes;
        int64_t sum = 0;
        for (int channel = 0; channel < channels; ++channel, p += sample_bytes) {
            int32_t sample;
            if (sample_bytes == 2) {
                sample = static_cast<int16_t>(p[0] | (p[1] << 8));
            } else if (sample_bytes == 3) {
                sample = static_cast<int16_t>(p[1] | (p[2] << 8));
            } else {
                sample = static_cast<int16_t>(p[2] | (p[3] << 8));
            }
            sum += sample;
        }
        out.push_back(static_cast<int16_t>(sum / channels));
    }
    return frames;
}

std::string HexEncode(const uint8_t* data, size_t size) {
    static const char kHex[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        hex.push_back(kHex[data[i] >> 4]);
        hex.push_back(kHex[data[i] & 0x0F]);
    }
    return hex;
}
