#include "custom_lcd_display.h"

#include <cstring>

#include "application.h"

#if CONFIG_USE_MUSIC_PLAYER
#include "cJSON.h"
#include "music_ui_model.h"

namespace {

void SetLabelText(lv_obj_t* label, const char* text) {
    if (std::strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

void StyleMusicCard(lv_obj_t* card) {
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(card, lv_color_black(), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 8, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
}

void StyleCircle(lv_obj_t* circle, int size, lv_color_t color) {
    lv_obj_set_size(circle, size, size);
    lv_obj_center(circle);
    lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(circle, color, 0);
    lv_obj_set_style_bg_opa(circle, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(circle, 0, 0);
    lv_obj_set_style_pad_all(circle, 0, 0);
    lv_obj_remove_flag(circle, LV_OBJ_FLAG_SCROLLABLE);
}

const char* JsonString(const cJSON* object, const char* name, const char* fallback = "") {
    auto* value = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(value) ? value->valuestring : fallback;
}

uint32_t JsonMilliseconds(const cJSON* object, const char* name) {
    auto* value = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsNumber(value) && value->valuedouble > 0
               ? static_cast<uint32_t>(value->valuedouble * 1000)
               : 0;
}

const char* PlayModeText(const char* mode) {
    if (std::strcmp(mode, "repeat_one") == 0) {
        return "单曲循环";
    }
    if (std::strcmp(mode, "repeat_all") == 0) {
        return "列表循环";
    }
    if (std::strcmp(mode, "shuffle") == 0) {
        return "随机播放";
    }
    return "顺序播放";
}

}  // namespace

void CustomLcdDisplay::SetupMusicUI() {
    // The stock network/battery/status bar stays above this page. This panel
    // covers the chat/emoji area and uses the existing theme's Chinese font.
    music_page_ = lv_obj_create(lv_display_get_screen_active(display_));
    lv_obj_set_pos(music_page_, 0, 36);
    lv_obj_set_size(music_page_, width_, height_ - 36);
    lv_obj_set_style_bg_color(music_page_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(music_page_, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(music_page_, lv_color_white(), 0);
    lv_obj_set_style_border_width(music_page_, 0, 0);
    lv_obj_set_style_radius(music_page_, 0, 0);
    lv_obj_set_style_pad_all(music_page_, 0, 0);
    lv_obj_remove_flag(music_page_, LV_OBJ_FLAG_SCROLLABLE);

    auto* vinyl_card = lv_obj_create(music_page_);
    lv_obj_set_pos(vinyl_card, 12, 8);
    lv_obj_set_size(vinyl_card, 140, 144);
    StyleMusicCard(vinyl_card);
    auto* disc = lv_obj_create(vinyl_card);
    StyleCircle(disc, 114, lv_color_black());
    for (int diameter : {88, 66, 44}) {
        auto* ring = lv_obj_create(disc);
        StyleCircle(ring, diameter, lv_color_black());
        lv_obj_set_style_border_color(ring, lv_color_white(), 0);
        lv_obj_set_style_border_width(ring, 1, 0);
    }
    auto* center = lv_obj_create(disc);
    StyleCircle(center, 26, lv_color_white());
    auto* hole = lv_obj_create(center);
    StyleCircle(hole, 8, lv_color_black());

    auto* info_card = lv_obj_create(music_page_);
    lv_obj_set_pos(info_card, 164, 8);
    lv_obj_set_size(info_card, width_ - 176, 144);
    StyleMusicCard(info_card);
    music_title_label_ = lv_label_create(info_card);
    lv_obj_set_pos(music_title_label_, 0, 0);
    lv_obj_set_size(music_title_label_, width_ - 196, 50);
    lv_label_set_long_mode(music_title_label_, LV_LABEL_LONG_WRAP);
    lv_label_set_text(music_title_label_, "未播放");
    music_artist_label_ = lv_label_create(info_card);
    lv_obj_set_pos(music_artist_label_, 0, 54);
    lv_obj_set_width(music_artist_label_, width_ - 196);
    lv_label_set_long_mode(music_artist_label_, LV_LABEL_LONG_DOT);
    lv_label_set_text(music_artist_label_, "未知歌手");
    music_state_label_ = lv_label_create(info_card);
    lv_obj_set_pos(music_state_label_, 0, 82);
    lv_obj_set_width(music_state_label_, width_ - 196);
    lv_label_set_text(music_state_label_, "正在播放\n顺序播放");

    music_progress_bar_ = lv_bar_create(music_page_);
    lv_obj_set_pos(music_progress_bar_, 12, 160);
    lv_obj_set_size(music_progress_bar_, width_ - 178, 10);
    lv_bar_set_range(music_progress_bar_, 0, 1000);
    lv_bar_set_value(music_progress_bar_, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(music_progress_bar_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(music_progress_bar_, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(music_progress_bar_, 2, 0);
    lv_obj_set_style_bg_color(music_progress_bar_, lv_color_black(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(music_progress_bar_, LV_OPA_COVER, LV_PART_INDICATOR);
    music_progress_label_ = lv_label_create(music_page_);
    lv_obj_set_pos(music_progress_label_, width_ - 154, 154);
    lv_obj_set_width(music_progress_label_, 142);
    lv_obj_set_style_text_align(music_progress_label_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(music_progress_label_, "00:00 / --:--");

    auto* lyric_card = lv_obj_create(music_page_);
    lv_obj_set_pos(lyric_card, 12, 180);
    lv_obj_set_size(lyric_card, width_ - 24, 34);
    StyleMusicCard(lyric_card);
    lv_obj_set_style_pad_all(lyric_card, 4, 0);
    music_lyric_label_ = lv_label_create(lyric_card);
    lv_obj_set_width(music_lyric_label_, width_ - 40);
    lv_obj_set_style_text_align(music_lyric_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(music_lyric_label_, LV_LABEL_LONG_DOT);
    lv_obj_center(music_lyric_label_);
    lv_label_set_text(music_lyric_label_, "暂无歌词");

    auto* controls = lv_label_create(music_page_);
    lv_obj_set_pos(controls, 12, 218);
    lv_obj_set_width(controls, width_ - 24);
    lv_label_set_text(controls, "KEY：单击暂停/继续 · 双击下一首\n三击切换模式 · 长按停止");
    lv_obj_add_flag(music_page_, LV_OBJ_FLAG_HIDDEN);
}

void CustomLcdDisplay::RefreshMusicUI() {
    if (music_page_ == nullptr) {
        return;
    }
    auto& app = Application::GetInstance();
    auto& player = app.GetMusicPlayer();
    // GetStatusJson takes the player mutex only while copying a small status
    // snapshot. Do not hold it while acquiring the LVGL lock.
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> status(player.GetStatusJson(), cJSON_Delete);
    if (!status) {
        return;
    }
    const char* state = JsonString(status.get(), "state");
    const bool paused = std::strcmp(state, "paused") == 0;
    const bool has_session = paused || std::strcmp(state, "playing") == 0;
    const bool visible =
        rlcd_music_ui::ShouldShowMusicPage(app.GetDeviceState(), has_session, paused);
    const uint32_t session_id = player.session_id();
    DisplayLockGuard lock(this);
    if (!lock) {
        return;
    }
    if (visible != music_page_visible_) {
        music_page_visible_ = visible;
        if (visible) {
            lv_obj_remove_flag(music_page_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(music_page_);
            lv_obj_move_foreground(top_bar_);
            lv_obj_move_foreground(status_bar_);
            lv_obj_move_foreground(low_battery_popup_);
            if (gif_controller_) {
                gif_controller_->Stop();
            }
        } else {
            lv_obj_add_flag(music_page_, LV_OBJ_FLAG_HIDDEN);
            if (gif_controller_) {
                gif_controller_->Start();
            }
        }
        ESP_LOGI("MusicUI", "Page: %s", visible ? "music" : "assistant");
    }
    if (!visible) {
        return;
    }
    auto* track = cJSON_GetObjectItemCaseSensitive(status.get(), "track");
    if (session_id != music_ui_session_id_) {
        music_ui_session_id_ = session_id;
        SetLabelText(music_lyric_label_, "暂无歌词");
    }
    SetLabelText(music_title_label_, JsonString(track, "title", "未知歌曲"));
    const char* artist = JsonString(track, "artist");
    SetLabelText(music_artist_label_, *artist == '\0' ? "未知歌手" : artist);
    std::string state_text = paused ? "已暂停\n" : "正在播放\n";
    state_text += PlayModeText(JsonString(status.get(), "play_mode"));
    SetLabelText(music_state_label_, state_text.c_str());
    const uint32_t position = JsonMilliseconds(status.get(), "position_s");
    const uint32_t duration = JsonMilliseconds(track, "duration_s");
    const int progress = rlcd_music_ui::MusicProgressPermille(position, duration);
    if (lv_bar_get_value(music_progress_bar_) != progress) {
        lv_bar_set_value(music_progress_bar_, progress, LV_ANIM_OFF);
    }
    auto timeline = rlcd_music_ui::FormatMusicTimeline(
        position, duration, cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(track, "live")));
    SetLabelText(music_progress_label_, timeline.c_str());
}
#endif  // CONFIG_USE_MUSIC_PLAYER

void CustomLcdDisplay::SetupUI() {
    if (IsSetupUICalled()) {
        return;
    }
    LcdDisplay::SetupUI();
#if CONFIG_USE_MUSIC_PLAYER
    DisplayLockGuard lock(this);
    if (lock) {
        SetupMusicUI();
    }
#endif
}

void CustomLcdDisplay::SetStatus(const char* status) {
#if CONFIG_USE_MUSIC_PLAYER
    RefreshMusicUI();
    if (music_page_visible_) {
        status = Application::GetInstance().GetMusicPlayer().IsPaused() ? "音乐已暂停" : "音乐播放";
    }
#endif
    LvglDisplay::SetStatus(status);
}

void CustomLcdDisplay::SetChatMessage(const char* role, const char* content) {
#if CONFIG_USE_MUSIC_PLAYER
    if (Application::GetInstance().GetDeviceState() == kDeviceStatePlaying) {
        RefreshMusicUI();
        MusicTrack track;
        if (music_lyric_label_ != nullptr &&
            Application::GetInstance().GetMusicPlayer().GetCurrentTrack(track)) {
            const std::string info =
                track.artist.empty() ? track.title : track.title + " - " + track.artist;
            // ShowMusicTrack still supplies the stock display's song summary;
            // subsequent assistant messages during playback are synced lyrics.
            if (role != nullptr && std::strcmp(role, "assistant") == 0 && content != nullptr &&
                info != content) {
                DisplayLockGuard lock(this);
                if (lock) {
                    SetLabelText(music_lyric_label_, *content == '\0' ? "暂无歌词" : content);
                }
            }
        }
        return;
    }
#endif
    LcdDisplay::SetChatMessage(role, content);
}

void CustomLcdDisplay::SetEmotion(const char* emotion) {
#if CONFIG_USE_MUSIC_PLAYER
    RefreshMusicUI();
    if (music_page_visible_) {
        return;
    }
#endif
    LcdDisplay::SetEmotion(emotion);
}

void CustomLcdDisplay::UpdateStatusBar(bool update_all) {
#if CONFIG_USE_MUSIC_PLAYER
    // The application's existing one-second tick updates progress without an
    // extra polling task or timer. Only changed labels invalidate the panel.
    RefreshMusicUI();
#endif
    LvglDisplay::UpdateStatusBar(update_all);
}
