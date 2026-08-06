#include "bsp/esp-bsp.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"

static void show_about(lv_event_t *event)
{
    lv_obj_t *status = lv_event_get_user_data(event);
    lv_label_set_text_fmt(status, "ESP32-P4  |  %lu KB free", (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024));
}

void app_main(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI("tab5-os", "Booted on %d-core ESP32-P4", chip.cores);

    bsp_display_start();
    bsp_display_lock(0);

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x10141f), 0);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "Tab5 OS");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 48, 40);

    lv_obj_t *subtitle = lv_label_create(screen);
    lv_label_set_text(subtitle, "A tiny open operating environment for M5Stack Tab5");
    lv_obj_set_style_text_color(subtitle, lv_color_hex(0x9aa7bd), 0);
    lv_obj_align_to(subtitle, title, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 12);

    lv_obj_t *status = lv_label_create(screen);
    lv_label_set_text(status, "Touch About to verify input");
    lv_obj_set_style_text_color(status, lv_color_hex(0x9aa7bd), 0);
    lv_obj_align(status, LV_ALIGN_BOTTOM_LEFT, 48, -40);

    lv_obj_t *about = lv_button_create(screen);
    lv_obj_set_size(about, 240, 96);
    lv_obj_align(about, LV_ALIGN_CENTER, 0, 30);
    lv_obj_add_event_cb(about, show_about, LV_EVENT_CLICKED, status);

    lv_obj_t *about_label = lv_label_create(about);
    lv_label_set_text(about_label, "About");
    lv_obj_center(about_label);

    bsp_display_unlock();
    bsp_display_backlight_on();
}
