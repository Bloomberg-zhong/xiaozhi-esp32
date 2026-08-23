#include "custom_lcd_display.h"

#include <string>
#include <vector>

#include "dashboard_model.h"
#include "lvgl_theme.h"

namespace {

constexpr int kScreenWidth = 400;
constexpr int kScreenHeight = 300;

void StyleMusicCard(lv_obj_t* card, int radius = 12) {
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_radius(card, radius, 0);
    lv_obj_set_style_pad_all(card, 8, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
}

void StyleCircle(lv_obj_t* circle, lv_color_t color, int border_width = 0) {
    lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(circle, color, 0);
    lv_obj_set_style_bg_opa(circle, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(circle, lv_color_white(), 0);
    lv_obj_set_style_border_width(circle, border_width, 0);
    lv_obj_set_style_pad_all(circle, 0, 0);
    lv_obj_remove_flag(circle, LV_OBJ_FLAG_SCROLLABLE);
}

std::vector<std::string> SplitLyricLines(const char* text) {
    std::vector<std::string> lines;
    const std::string source = text != nullptr ? text : "";
    size_t start = 0;
    while (start <= source.size() && lines.size() < 3) {
        size_t end = source.find('\n', start);
        if (end == std::string::npos) {
            end = source.size();
        }
        lines.push_back(source.substr(start, end - start));
        if (end == source.size()) {
            break;
        }
        start = end + 1;
    }
    return lines;
}

}  // namespace

void CustomLcdDisplay::SetupMusicUI() {
    auto* screen = lv_screen_active();
    auto* theme = static_cast<LvglTheme*>(current_theme_);
    auto* text_font = theme->text_font()->font();

    music_page_ = lv_obj_create(screen);
    lv_obj_set_pos(music_page_, 0, 0);
    lv_obj_set_size(music_page_, kScreenWidth, kScreenHeight);
    lv_obj_set_style_bg_color(music_page_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(music_page_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(music_page_, 0, 0);
    lv_obj_set_style_radius(music_page_, 0, 0);
    lv_obj_set_style_pad_all(music_page_, 0, 0);
    lv_obj_remove_flag(music_page_, LV_OBJ_FLAG_SCROLLABLE);

    auto* vinyl_card = lv_obj_create(music_page_);
    lv_obj_set_pos(vinyl_card, 12, 38);
    lv_obj_set_size(vinyl_card, 142, 142);
    StyleMusicCard(vinyl_card, 14);

    auto* vinyl_disc = lv_obj_create(vinyl_card);
    lv_obj_set_size(vinyl_disc, 122, 122);
    lv_obj_center(vinyl_disc);
    StyleCircle(vinyl_disc, lv_color_black(), 2);

    constexpr int kRingSizes[] = {96, 74, 54};
    for (const int ring_size : kRingSizes) {
        auto* ring = lv_obj_create(vinyl_disc);
        lv_obj_set_size(ring, ring_size, ring_size);
        lv_obj_center(ring);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(ring, lv_color_white(), 0);
        lv_obj_set_style_border_width(ring, 1, 0);
        lv_obj_set_style_pad_all(ring, 0, 0);
        lv_obj_remove_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
    }

    auto* vinyl_center = lv_obj_create(vinyl_disc);
    lv_obj_set_size(vinyl_center, 38, 38);
    lv_obj_center(vinyl_center);
    StyleCircle(vinyl_center, lv_color_white());

    auto* vinyl_hole = lv_obj_create(vinyl_center);
    lv_obj_set_size(vinyl_hole, 9, 9);
    lv_obj_center(vinyl_hole);
    StyleCircle(vinyl_hole, lv_color_black());

    auto* info_card = lv_obj_create(music_page_);
    lv_obj_set_pos(info_card, 166, 38);
    lv_obj_set_size(info_card, 222, 142);
    StyleMusicCard(info_card, 14);

    music_title_label_ = lv_label_create(info_card);
    lv_obj_set_width(music_title_label_, 198);
    lv_obj_set_style_text_font(music_title_label_, text_font, 0);
    lv_obj_set_style_text_color(music_title_label_, lv_color_black(), 0);
    lv_label_set_long_mode(music_title_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(music_title_label_, "未播放");
    lv_obj_align(music_title_label_, LV_ALIGN_TOP_LEFT, 0, 0);

    music_artist_label_ = lv_label_create(info_card);
    lv_obj_set_width(music_artist_label_, 198);
    lv_obj_set_style_text_font(music_artist_label_, text_font, 0);
    lv_obj_set_style_text_color(music_artist_label_, lv_color_black(), 0);
    lv_label_set_long_mode(music_artist_label_, LV_LABEL_LONG_DOT);
    lv_label_set_text(music_artist_label_, "未知歌手");
    lv_obj_align(music_artist_label_, LV_ALIGN_TOP_LEFT, 0, 24);

    auto* divider = lv_obj_create(info_card);
    lv_obj_set_pos(divider, 0, 48);
    lv_obj_set_size(divider, 198, 1);
    lv_obj_set_style_bg_color(divider, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(divider, 0, 0);
    lv_obj_set_style_pad_all(divider, 0, 0);
    lv_obj_remove_flag(divider, LV_OBJ_FLAG_SCROLLABLE);

    music_lyric_previous_label_ = lv_label_create(info_card);
    music_lyric_current_label_ = lv_label_create(info_card);
    music_lyric_next_label_ = lv_label_create(info_card);
    lv_obj_t* lyric_labels[] = {music_lyric_previous_label_, music_lyric_current_label_,
                                music_lyric_next_label_};
    for (int index = 0; index < 3; ++index) {
        lv_obj_set_width(lyric_labels[index], 198);
        lv_obj_set_style_text_font(lyric_labels[index], text_font, 0);
        lv_obj_set_style_text_color(lyric_labels[index], lv_color_black(), 0);
        lv_label_set_long_mode(lyric_labels[index],
                               index == 1 ? LV_LABEL_LONG_SCROLL_CIRCULAR : LV_LABEL_LONG_DOT);
        lv_obj_align(lyric_labels[index], LV_ALIGN_TOP_LEFT, 0, 54 + index * 25);
    }
    lv_label_set_text(music_lyric_previous_label_, "");
    lv_label_set_text(music_lyric_current_label_, "等待播放...");
    lv_label_set_text(music_lyric_next_label_, "");

    music_progress_bar_ = lv_bar_create(music_page_);
    lv_obj_set_pos(music_progress_bar_, 12, 194);
    lv_obj_set_size(music_progress_bar_, 274, 12);
    lv_bar_set_range(music_progress_bar_, 0, 1000);
    lv_bar_set_value(music_progress_bar_, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(music_progress_bar_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(music_progress_bar_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(music_progress_bar_, lv_color_white(), 0);
    lv_obj_set_style_border_width(music_progress_bar_, 1, 0);
    lv_obj_set_style_radius(music_progress_bar_, 6, 0);
    lv_obj_set_style_pad_all(music_progress_bar_, 2, 0);
    lv_obj_set_style_bg_color(music_progress_bar_, lv_color_black(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(music_progress_bar_, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(music_progress_bar_, 4, LV_PART_INDICATOR);

    music_progress_label_ = lv_label_create(music_page_);
    lv_obj_set_pos(music_progress_label_, 296, 190);
    lv_obj_set_width(music_progress_label_, 92);
    lv_obj_set_style_text_font(music_progress_label_, text_font, 0);
    lv_obj_set_style_text_color(music_progress_label_, lv_color_white(), 0);
    lv_obj_set_style_text_align(music_progress_label_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(music_progress_label_, "00:00 / --:--");

    auto* ai_card = lv_obj_create(music_page_);
    lv_obj_set_pos(ai_card, 12, 220);
    lv_obj_set_size(ai_card, 376, 70);
    StyleMusicCard(ai_card, 14);

    auto* ai_title = lv_label_create(ai_card);
    lv_obj_set_pos(ai_title, 4, 3);
    lv_obj_set_width(ai_title, 64);
    lv_obj_set_style_text_font(ai_title, text_font, 0);
    lv_obj_set_style_text_color(ai_title, lv_color_black(), 0);
    lv_obj_set_style_text_align(ai_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(ai_title, "小智");

    auto* ai_divider = lv_obj_create(ai_card);
    lv_obj_set_pos(ai_divider, 76, 0);
    lv_obj_set_size(ai_divider, 2, 50);
    lv_obj_set_style_bg_color(ai_divider, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ai_divider, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ai_divider, 0, 0);
    lv_obj_set_style_pad_all(ai_divider, 0, 0);
    lv_obj_remove_flag(ai_divider, LV_OBJ_FLAG_SCROLLABLE);

    music_ai_status_label_ = lv_label_create(ai_card);
    lv_obj_set_pos(music_ai_status_label_, 90, 4);
    lv_obj_set_width(music_ai_status_label_, 264);
    lv_obj_set_style_text_font(music_ai_status_label_, text_font, 0);
    lv_obj_set_style_text_color(music_ai_status_label_, lv_color_black(), 0);
    lv_label_set_long_mode(music_ai_status_label_, LV_LABEL_LONG_WRAP);
    lv_label_set_text(music_ai_status_label_, "待命 · 说出歌名即可播放");

    lv_obj_add_flag(music_page_, LV_OBJ_FLAG_HIDDEN);
}

void CustomLcdDisplay::SetMusicInfo(const char* title, const char* artist) {
    DisplayLockGuard lock(this);
    if (music_title_label_ != nullptr) {
        lv_label_set_text(music_title_label_, title != nullptr ? title : "未知歌曲");
    }
    if (music_artist_label_ != nullptr) {
        lv_label_set_text(music_artist_label_, artist != nullptr ? artist : "未知歌手");
    }
}

void CustomLcdDisplay::SetMusicLyric(const char* lyric) {
    DisplayLockGuard lock(this);
    if (music_lyric_current_label_ == nullptr) {
        return;
    }
    const auto lines = SplitLyricLines(lyric);
    if (lines.size() >= 3) {
        lv_label_set_text(music_lyric_previous_label_, lines[0].c_str());
        lv_label_set_text(music_lyric_current_label_, lines[1].c_str());
        lv_label_set_text(music_lyric_next_label_, lines[2].c_str());
    } else {
        lv_label_set_text(music_lyric_previous_label_, "");
        lv_label_set_text(music_lyric_current_label_, lines.empty() ? "" : lines[0].c_str());
        lv_label_set_text(music_lyric_next_label_, lines.size() > 1 ? lines[1].c_str() : "");
    }
}

void CustomLcdDisplay::SetMusicProgress(uint32_t current_ms, uint32_t total_ms) {
    DisplayLockGuard lock(this);
    if (music_progress_bar_ == nullptr || music_progress_label_ == nullptr) {
        return;
    }
    lv_bar_set_value(music_progress_bar_,
                     rlcd_dashboard::MusicProgressPermille(current_ms, total_ms), LV_ANIM_OFF);
    const std::string current = rlcd_dashboard::FormatPlaybackTime(current_ms);
    const std::string total =
        total_ms == 0 ? "--:--" : rlcd_dashboard::FormatPlaybackTime(total_ms);
    const std::string text = current + " / " + total;
    lv_label_set_text(music_progress_label_, text.c_str());
}
