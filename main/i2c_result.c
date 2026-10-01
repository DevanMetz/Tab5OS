#include "i2c_result.h"

#include <string.h>
#include <stdio.h>
#include "payload_clipboard.h"

static struct {
    size_t length;
    uint8_t address, reg, bytes[32];
    uint32_t speed_hz;
} saved;
static lv_obj_t *copy_button, *details;
static void (*cancel_write)(void);

static void render(const char *notice)
{
    if (!copy_button || !details) return;
    if (saved.length) {
        lv_obj_remove_state(copy_button, LV_STATE_DISABLED);
        char value[128];
        if (saved.length == 1) {
            snprintf(value, sizeof(value), "Byte: 0x%02X (%u)", saved.bytes[0], saved.bytes[0]);
        } else {
            size_t used = (size_t)snprintf(value, sizeof(value), "Bytes (%u):", (unsigned)saved.length);
            for (size_t i = 0; i < saved.length; i++)
                used += (size_t)snprintf(value + used, sizeof(value) - used, " %02X", saved.bytes[i]);
        }
        lv_label_set_text_fmt(details,
            "Saved read: addr 0x%02X | reg 0x%02X | %lu kHz\n"
            "%s\n%s",
            saved.address, saved.reg, (unsigned long)(saved.speed_hz / 1000),
            value, notice ? notice : "Copy uses this saved read; it does not read the bus.");
    } else {
        lv_obj_add_state(copy_button, LV_STATE_DISABLED);
        lv_label_set_text_fmt(details, "Saved read: none\n%s",
            notice ? notice : "Complete a read, then COPY READ to inspect it in Byte Lab.");
    }
}

void i2c_result_record(bool success, uint8_t address, uint8_t reg,
                       uint32_t speed_hz, const uint8_t *bytes, size_t length)
{
    memset(&saved, 0, sizeof(saved));
    if (success && bytes && length && length <= sizeof(saved.bytes)) {
        saved.length = length;
        saved.address = address;
        saved.reg = reg;
        saved.speed_hz = speed_hz;
        memcpy(saved.bytes, bytes, length);
    }
    render(NULL);
}

void i2c_result_clear(void)
{
    memset(&saved, 0, sizeof(saved));
    render("Saved read cleared; the clipboard is unchanged.");
}

static void copy_clicked(lv_event_t *event)
{
    (void)event;
    if (cancel_write) cancel_write();
    if (!saved.length) {
        render("Complete a read first; the previous clipboard is unchanged.");
        return;
    }
    if (payload_clipboard_store(saved.bytes, saved.length))
        render("Copied the complete read. Use Paste hex in Byte Lab to inspect it.");
    else render("Copy failed; the previous clipboard is unchanged.");
}

static void clear_clicked(lv_event_t *event)
{
    (void)event;
    if (cancel_write) cancel_write();
    i2c_result_clear();
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, lv_event_cb_t callback)
{
    lv_obj_t *object = lv_button_create(parent);
    lv_obj_set_size(object, 300, 64);
    lv_obj_set_style_text_font(object, &lv_font_montserrat_28, 0);
    lv_obj_add_event_cb(object, callback, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = lv_label_create(object);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return object;
}

void i2c_result_show(lv_obj_t *parent, void (*before_action)(void))
{
    cancel_write = before_action;
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, 620, 154);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(panel, 8, 0);
    lv_obj_t *row = lv_obj_create(panel);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 620, 66);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                         LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    copy_button = button(row, "COPY READ", copy_clicked);
    button(row, "CLEAR READ", clear_clicked);
    details = lv_label_create(panel);
    lv_obj_set_size(details, 620, 80);
    lv_obj_set_style_text_font(details, &lv_font_montserrat_14, 0);
    lv_label_set_long_mode(details, LV_LABEL_LONG_WRAP);
    render(NULL);
}

void i2c_result_stop(void)
{
    copy_button = details = NULL;
    cancel_write = NULL;
}
