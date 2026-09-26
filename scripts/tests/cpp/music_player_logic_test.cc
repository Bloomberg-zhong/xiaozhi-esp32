// Host tests for the pure music player helpers. Built and run by
// scripts/tests/test_music_player_logic.py.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>

#include "local_music.h"
#include "lrc_parser.h"
#include "music_util.h"

namespace {

int failures = 0;

#define EXPECT(condition)                                                              \
    do {                                                                               \
        if (!(condition)) {                                                            \
            std::printf("%s:%d: EXPECT(%s) failed\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                                \
        }                                                                              \
    } while (0)

void TestLrcParsing() {
    const std::string lrc =
        "[ti:Title]\r\n"
        "[ar:Artist]\n"
        "[00:12.50]second\n"
        "[00:01.00][00:20.123]first and repeated\n"
        "[00:05]  no fraction  \n"
        "[01:02:30]colon fraction\n"
        "[00:07.5]one digit\n"
        "[00:08.00]\n"
        "not a lyric\n"
        "[99:99.00]invalid seconds\n";
    auto lines = ParseLrc(lrc);
    EXPECT(lines.size() == 7);
    if (lines.size() == 7) {
        EXPECT(lines[0].time_ms == 1000 && lines[0].text == "first and repeated");
        EXPECT(lines[1].time_ms == 5000 && lines[1].text == "no fraction");
        EXPECT(lines[2].time_ms == 7500 && lines[2].text == "one digit");
        EXPECT(lines[3].time_ms == 8000 && lines[3].text.empty());
        EXPECT(lines[4].time_ms == 12500 && lines[4].text == "second");
        EXPECT(lines[5].time_ms == 20123 && lines[5].text == "first and repeated");
        EXPECT(lines[6].time_ms == 62300 && lines[6].text == "colon fraction");
    }

    EXPECT(FindLyricIndex(lines, 0) == -1);
    EXPECT(FindLyricIndex(lines, 999) == -1);
    EXPECT(FindLyricIndex(lines, 1000) == 0);
    EXPECT(FindLyricIndex(lines, 12499) == 3);
    EXPECT(FindLyricIndex(lines, 12500) == 4);
    EXPECT(FindLyricIndex(lines, 600000) == 6);
    EXPECT(FindLyricIndex({}, 1000) == -1);
}

void TestLrcOffsetAndLimit() {
    auto lines = ParseLrc("[offset:500]\n[00:01.00]a\n[00:00.20]b\n");
    EXPECT(lines.size() == 2);
    if (lines.size() == 2) {
        EXPECT(lines[0].time_ms == 0 && lines[0].text == "b");  // Clamped at zero
        EXPECT(lines[1].time_ms == 500 && lines[1].text == "a");
    }
    lines = ParseLrc("[offset:-250]\n[00:01.00]a\n");
    EXPECT(lines.size() == 1 && lines[0].time_ms == 1250);

    std::string many;
    for (int i = 0; i < 100; ++i) {
        many += "[00:01.00]x\n";
    }
    EXPECT(ParseLrc(many, 10).size() == 10);
    EXPECT(ParseLrc("").empty());
    EXPECT(ParseLrc("plain text without tags").empty());
}

void TestFormatDetection() {
    const uint8_t id3[] = {'I', 'D', '3', 4, 0};
    const uint8_t mp3_frame[] = {0xFF, 0xFB, 0x90, 0x64};
    const uint8_t adts[] = {0xFF, 0xF1, 0x50, 0x80};
    const uint8_t flac[] = {'f', 'L', 'a', 'C', 0, 0};
    const uint8_t wav[] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E'};
    const uint8_t m4a[] = {0, 0, 0, 0x20, 'f', 't', 'y', 'p', 'M', '4', 'A', ' '};
    const uint8_t junk[] = {1, 2, 3, 4};

    EXPECT(DetectMusicAudioFormat(id3, sizeof(id3), "", "") == MusicAudioFormat::kMp3);
    EXPECT(DetectMusicAudioFormat(mp3_frame, sizeof(mp3_frame), "", "") == MusicAudioFormat::kMp3);
    EXPECT(DetectMusicAudioFormat(adts, sizeof(adts), "", "") == MusicAudioFormat::kAac);
    EXPECT(DetectMusicAudioFormat(flac, sizeof(flac), "", "") == MusicAudioFormat::kFlac);
    EXPECT(DetectMusicAudioFormat(wav, sizeof(wav), "", "") == MusicAudioFormat::kWav);
    EXPECT(DetectMusicAudioFormat(m4a, sizeof(m4a), "", "") == MusicAudioFormat::kM4a);
    // Magic bytes win over headers.
    EXPECT(DetectMusicAudioFormat(flac, sizeof(flac), "audio/mpeg", "a.mp3") ==
           MusicAudioFormat::kFlac);
    EXPECT(DetectMusicAudioFormat(junk, sizeof(junk), "Audio/MPEG; charset=binary", "") ==
           MusicAudioFormat::kMp3);
    EXPECT(DetectMusicAudioFormat(junk, sizeof(junk), "application/octet-stream",
                                  "http://h/music/song.FLAC?token=1") == MusicAudioFormat::kFlac);
    EXPECT(DetectMusicAudioFormat(junk, sizeof(junk), "", "http://h/a.dir/stream") ==
           MusicAudioFormat::kUnknown);
    EXPECT(DetectMusicAudioFormat(nullptr, 0, "", "") == MusicAudioFormat::kUnknown);
}

void TestUrls() {
    EXPECT(IsHttpUrl("http://a"));
    EXPECT(IsHttpUrl("HTTPS://a"));
    EXPECT(!IsHttpUrl("http://"));
    EXPECT(!IsHttpUrl("ftp://a"));
    EXPECT(!IsHttpUrl("/relative"));

    EXPECT(UrlEncode("稻香 周杰伦&a=b") ==
           "%E7%A8%BB%E9%A6%99%20%E5%91%A8%E6%9D%B0%E4%BC%A6%26a%3Db");
    EXPECT(UrlEncode("AZaz09-_.~") == "AZaz09-_.~");

    EXPECT(BuildUrl("http://h:4533/", "rest/ping.view", {{"u", "me"}, {"q", "a b"}}) ==
           "http://h:4533/rest/ping.view?u=me&q=a%20b");
    EXPECT(BuildUrl("http://h/api", "/search", {}) == "http://h/api/search");
    EXPECT(BuildUrl("http://h/x?k=1", "", {{"a", "2"}}) == "http://h/x?k=1&a=2");

    EXPECT(UrlOrigin("https://h.example:8443/a/b?c") == "https://h.example:8443");
    EXPECT(UrlOrigin("http://h") == "http://h");
    EXPECT(UrlOrigin("file:///x").empty());

    EXPECT(ResolveUrl("http://h/api", "https://cdn/x.mp3") == "https://cdn/x.mp3");
    EXPECT(ResolveUrl("http://h/api", "/files/x.mp3") == "http://h/files/x.mp3");
    EXPECT(ResolveUrl("http://h/api/", "files/x.mp3") == "http://h/api/files/x.mp3");
    EXPECT(ResolveUrl("http://h/api?x=1", "files/x.mp3") == "http://h/api/files/x.mp3");
    EXPECT(ResolveUrl("http://h/api", "").empty());

    const uint8_t bytes[] = {0x00, 0x7f, 0xab, 0xff};
    EXPECT(HexEncode(bytes, sizeof(bytes)) == "007fabff");
}

void TestDownmix() {
    std::vector<int16_t> out;
    const int16_t stereo[] = {100, 300, -200, -400, 32767, 32767};
    EXPECT(DownmixToMono16(reinterpret_cast<const uint8_t*>(stereo), sizeof(stereo), 2, 16, out) ==
           3);
    EXPECT((out == std::vector<int16_t>{200, -300, 32767}));

    out.clear();
    const int16_t mono[] = {1, -2, 3};
    // A trailing partial frame is ignored.
    EXPECT(DownmixToMono16(reinterpret_cast<const uint8_t*>(mono), sizeof(mono) + 1 - 1, 1, 16,
                           out) == 3);
    EXPECT((out == std::vector<int16_t>{1, -2, 3}));

    out.clear();
    // 24-bit little endian: 0x123456 -> 0x1234, 0xFEDCBA -> 0xFEDC.
    const uint8_t pcm24[] = {0x56, 0x34, 0x12, 0xBA, 0xDC, 0xFE};
    EXPECT(DownmixToMono16(pcm24, sizeof(pcm24), 1, 24, out) == 2);
    EXPECT((out == std::vector<int16_t>{0x1234, static_cast<int16_t>(0xFEDC)}));

    out.clear();
    const int32_t pcm32[] = {0x40000000, -0x40000000};
    EXPECT(DownmixToMono16(reinterpret_cast<const uint8_t*>(pcm32), sizeof(pcm32), 2, 32, out) ==
           1);
    EXPECT((out == std::vector<int16_t>{0}));

    out.clear();
    EXPECT(DownmixToMono16(pcm24, sizeof(pcm24), 1, 8, out) == 0);
    EXPECT(DownmixToMono16(pcm24, sizeof(pcm24), 0, 16, out) == 0);
    EXPECT(out.empty());
}

void WriteFile(const std::string& path, const std::string& content) {
    std::ofstream(path, std::ios::binary) << content;
}

void TestLocalMusic() {
    char root_template[] = "/tmp/xiaozhi_music_XXXXXX";
    const char* root_ptr = mkdtemp(root_template);
    EXPECT(root_ptr != nullptr);
    if (root_ptr == nullptr) {
        return;
    }
    const std::string root = root_ptr;
    mkdir((root + "/儿童音乐").c_str(), 0755);
    mkdir((root + "/儿童音乐/deep").c_str(), 0755);
    mkdir((root + "/White-Noise").c_str(), 0755);
    mkdir((root + "/.hidden").c_str(), 0755);
    WriteFile(root + "/儿童音乐/小星星.mp3", "x");
    WriteFile(root + "/儿童音乐/小星星.lrc", "[00:01.00]一闪一闪");
    WriteFile(root + "/儿童音乐/deep/Beyond - 海阔天空.FLAC", "x");
    WriteFile(root + "/儿童音乐/notes.txt", "x");
    WriteFile(root + "/White-Noise/rain.mp3", "x");
    WriteFile(root + "/.hidden/secret.mp3", "x");
    WriteFile(root + "/a.wav", "x");

    auto all = ScanLocalMusic(root + "/");
    EXPECT(all.size() == 4);  // Includes white noise, skips hidden and non-audio files
    if (all.size() == 4) {
        EXPECT(all[0].stream_url == root + "/a.wav");  // Files before sub folders
        EXPECT(all[0].album == root.substr(root.rfind('/') + 1));
    }

    LocalMusicScanOptions options;
    options.excluded_folders.push_back(kWhiteNoiseFolder);
    auto music = ScanLocalMusic(root, options);
    EXPECT(music.size() == 3);
    if (music.size() == 3) {
        EXPECT(music[1].title == "小星星" && music[1].artist.empty());
        EXPECT(music[1].album == "儿童音乐");
        EXPECT(music[1].lyric_url == root + "/儿童音乐/小星星.lrc");
        EXPECT(music[2].title == "海阔天空" && music[2].artist == "Beyond");
        EXPECT(music[2].album == "deep" && music[2].lyric_url.empty());
        EXPECT(IsLocalMusicPath(music[2].stream_url));
    }

    options.max_depth = 0;
    EXPECT(ScanLocalMusic(root, options).size() == 1);
    options.max_depth = 4;
    options.max_tracks = 2;
    EXPECT(ScanLocalMusic(root, options).size() == 2);

    EXPECT(FilterLocalMusic(music, "").size() == 3);
    EXPECT(FilterLocalMusic(music, "儿童音乐").size() == 2);
    EXPECT(FilterLocalMusic(music, "beyond 海阔").size() == 1);
    EXPECT(FilterLocalMusic(music, "beyond 小星星").empty());
    EXPECT(ScanLocalMusic(root + "/missing").empty());
    EXPECT(!IsLocalMusicPath("http://h/a.mp3"));

    std::string text;
    EXPECT(ReadLocalTextFile(root + "/儿童音乐/小星星.lrc", 1024, text));
    EXPECT(text == "[00:01.00]一闪一闪");
    EXPECT(!ReadLocalTextFile(root + "/儿童音乐/小星星.lrc", 4, text));
    EXPECT(!ReadLocalTextFile(root + "/missing.lrc", 1024, text));

    std::system(("rm -rf '" + root + "'").c_str());
}

}  // namespace

int main() {
    TestLrcParsing();
    TestLrcOffsetAndLimit();
    TestFormatDetection();
    TestUrls();
    TestDownmix();
    TestLocalMusic();
    if (failures != 0) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("All music player logic tests passed\n");
    return 0;
}
