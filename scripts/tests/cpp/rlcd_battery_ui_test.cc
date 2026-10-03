#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

struct lv_obj_t {
    lv_obj_t* parent = nullptr;
    std::vector<lv_obj_t*> children;
    std::string text;
    int width = 0, align = 0;
    bool hidden = false;
    int writes = 0;
};
constexpr int LV_TEXT_ALIGN_RIGHT = 1, LV_OBJ_FLAG_HIDDEN = 2;
constexpr int lv_font_montserrat_14 = 14;
lv_obj_t* lv_obj_get_parent(lv_obj_t* object) { return object->parent; }
int lv_obj_get_index(lv_obj_t* object) {
    auto& siblings = object->parent->children;
    return std::find(siblings.begin(), siblings.end(), object) - siblings.begin();
}
void lv_obj_move_to_index(lv_obj_t* object, int index) {
    auto& siblings = object->parent->children;
    siblings.erase(std::find(siblings.begin(), siblings.end(), object));
    siblings.insert(siblings.begin() + index, object);
}
void lv_obj_set_style_pad_column(lv_obj_t*, int, int) {}
void lv_obj_set_style_text_font(lv_obj_t*, const int*, int) {}
void lv_obj_set_style_text_align(lv_obj_t* object, int align, int) { object->align = align; }
void lv_obj_set_width(lv_obj_t* object, int width) { object->width = width; }
void lv_obj_add_flag(lv_obj_t* object, int) { object->hidden = true; }
void lv_obj_remove_flag(lv_obj_t* object, int) { object->hidden = false; }
bool lv_obj_has_flag(lv_obj_t* object, int) { return object->hidden; }
const char* lv_label_get_text(lv_obj_t* object) { return object->text.c_str(); }
void lv_label_set_text(lv_obj_t* object, const char* text) {
    object->text = text;
    ++object->writes;
}
lv_obj_t* Label(lv_obj_t* parent, int, int, int width, const char* text) {
    auto* label = new lv_obj_t;
    label->parent = parent;
    label->width = width;
    label->text = text;
    parent->children.push_back(label);
    return label;
}
class CustomLcdDisplay {
public:
    lv_obj_t* battery_label_ = nullptr;
    lv_obj_t* battery_percentage_label_ = nullptr;
    lv_obj_t* status_label_ = nullptr;
    lv_obj_t* notification_label_ = nullptr;
    int width_ = 400;
    void SetupBatteryPercentageUI();
    void RefreshBatteryPercentage(bool available, int level);
};

// PRODUCTION_METHODS

int main() {
    CustomLcdDisplay no_battery;
    no_battery.SetupBatteryPercentageUI();
    no_battery.RefreshBatteryPercentage(false, 98);
    assert(!no_battery.battery_percentage_label_);

    lv_obj_t icons, status, notification;
    auto* icon = Label(&icons, 0, 0, 30, "battery");
    CustomLcdDisplay display;
    display.battery_label_ = icon;
    display.status_label_ = &status;
    display.notification_label_ = &notification;
    display.SetupBatteryPercentageUI();
    auto* percent = display.battery_percentage_label_;
    assert(percent && percent->parent == icon->parent);
    assert(lv_obj_get_index(percent) + 1 == lv_obj_get_index(icon));
    assert(status.width <= 160 && notification.width <= 160);
    display.RefreshBatteryPercentage(true, 98);
    assert(percent->text == "98%" && !percent->hidden);
    auto writes = percent->writes;
    display.RefreshBatteryPercentage(true, 98);
    assert(percent->writes == writes);  // Avoid repainting unchanged percentages.
    display.RefreshBatteryPercentage(true, 100);
    assert(percent->text == "100%");
    display.RefreshBatteryPercentage(true, 0);
    assert(percent->text == "0%");
    display.RefreshBatteryPercentage(true, -10);
    assert(percent->text == "0%");
    display.RefreshBatteryPercentage(true, 150);
    assert(percent->text == "100%");
    display.RefreshBatteryPercentage(false, 98);
    assert(percent->hidden && percent->text.empty());
    display.RefreshBatteryPercentage(true, 98);
    assert(!percent->hidden && percent->text == "98%");
    delete percent;
    delete icon;
}
