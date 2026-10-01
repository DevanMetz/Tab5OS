#include "subnet_tool.h"

#include <stdio.h>
#include <string.h>

#include "subnet_data.h"

/* LVGL limits Unicode characters, so retain even rejected UTF-8 input intact. */
static char inputs[3][SUBNET_INPUT_MAX * 4 + 1] = {
    "192.168.1.42", "24", "192.168.1.100"
};
static lv_obj_t *fields[3];
static lv_obj_t *keyboard;
static lv_obj_t *keys_label;
static lv_obj_t *result_label;
static lv_obj_t *peer_label;
static lv_obj_t *note_label;
static bool updating;

static const char *const key_map[] = {
    "1", "2", "3", "4", "5", "\n",
    "6", "7", "8", "9", "0", "\n",
    ".", "/", LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT, LV_SYMBOL_BACKSPACE, LV_SYMBOL_OK, ""
};
static const lv_buttonmatrix_ctrl_t key_controls[] = {
    2, 2, 2, 2, 2,
    2, 2, 2, 2, 2,
    2, 2, LV_BUTTONMATRIX_CTRL_CHECKED | 1, LV_BUTTONMATRIX_CTRL_CHECKED | 1,
    LV_BUTTONMATRIX_CTRL_CHECKED | 2, LV_KEYBOARD_CTRL_BUTTON_FLAGS | 2
};
_Static_assert(sizeof(key_map) / sizeof(key_map[0]) ==
               sizeof(key_controls) / sizeof(key_controls[0]) + 3,
               "Subnet keyboard map and controls must match");

static void calculate(void)
{
    if (!result_label) return;
    subnet_data_t data;
    subnet_status_t status = subnet_calculate(inputs[0], inputs[1], inputs[2], &data);
    lv_label_set_text(peer_label, "");
    lv_label_set_text(note_label, "");
    if (status != SUBNET_OK) {
        lv_label_set_text(result_label, subnet_error(status));
        return;
    }

    char network[16], mask[16], wildcard[16], last[16], first_host[16], last_host[16];
    subnet_format_ipv4(data.network, network);
    subnet_format_ipv4(data.mask, mask);
    subnet_format_ipv4(data.wildcard, wildcard);
    subnet_format_ipv4(data.last_address, last);
    subnet_format_ipv4(data.first_host, first_host);
    subnet_format_ipv4(data.last_host, last_host);
    char text[512];
    snprintf(text, sizeof(text),
             "Network: %s/%u\nNetmask: %s\nWildcard: %s\nBroadcast: %s\n"
             "First host slot: %s\nLast host slot: %s\n"
             "Total addresses: %llu\nHost slots: %llu",
             network, data.prefix, mask, wildcard,
             data.has_broadcast ? last : "none",
             first_host, last_host,
             (unsigned long long)data.address_count, (unsigned long long)data.host_count);
    lv_label_set_text(result_label, text);

    if (data.has_peer) {
        const char *membership = data.peer_role == SUBNET_OUTSIDE ? "outside this subnet" :
            data.peer_role == SUBNET_NETWORK_ADDRESS ? "inside; network address" :
            data.peer_role == SUBNET_BROADCAST_ADDRESS ? "inside; broadcast address" :
            "inside; host slot";
        snprintf(text, sizeof(text), "Peer %s:\n%s", inputs[2], membership);
        lv_label_set_text(peer_label, text);
    } else {
        lv_label_set_text(peer_label, "Enter a peer to check subnet membership.");
    }
    const char *note = data.prefix == 31 ?
        "/31: two endpoints on a point-to-point link; no directed broadcast." :
        data.prefix == 32 ? "/32: a single address/host route; no directed broadcast." :
        data.address_role == SUBNET_NETWORK_ADDRESS ? "The input is the network address, not a host slot." :
        data.address_role == SUBNET_BROADCAST_ADDRESS ? "The input is the broadcast address, not a host slot." :
        "The input falls in a host slot for this mask.";
    lv_label_set_text(note_label, note);
}

static void set_keyboard_visible(bool visible)
{
    if (visible) lv_obj_remove_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(keys_label, visible ? "Hide keys" : "Show keys");
    if (!visible) {
        for (unsigned i = 0; i < 3; i++) lv_obj_remove_state(fields[i], LV_STATE_FOCUSED);
    }
}

static void calculate_clicked(lv_event_t *event)
{
    (void)event;
    calculate();
    set_keyboard_visible(false);
}

static void keys_clicked(lv_event_t *event)
{
    (void)event;
    set_keyboard_visible(lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN));
}

static void keyboard_done(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_READY) calculate();
    set_keyboard_visible(false);
}

static void field_event(lv_event_t *event)
{
    if (updating || !keyboard) return;
    lv_obj_t *field = lv_event_get_target_obj(event);
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED) {
        lv_keyboard_set_textarea(keyboard, field);
        set_keyboard_visible(true);
    } else if (code == LV_EVENT_VALUE_CHANGED) {
        unsigned index = (unsigned)(uintptr_t)lv_event_get_user_data(event);
        snprintf(inputs[index], sizeof(inputs[index]), "%s", lv_textarea_get_text(field));
        lv_label_set_text(result_label, "Inputs changed. Tap Calculate.");
        lv_label_set_text(peer_label, "");
        lv_label_set_text(note_label, "");
    }
}

static void example_clicked(lv_event_t *event)
{
    (void)event;
    static const char *const examples[] = {"192.168.1.42", "24", "192.168.1.100"};
    updating = true;
    for (unsigned i = 0; i < 3; i++) {
        snprintf(inputs[i], sizeof(inputs[i]), "%s", examples[i]);
        lv_textarea_set_text(fields[i], inputs[i]);
    }
    updating = false;
    calculate();
    set_keyboard_visible(false);
}

static lv_obj_t *row(lv_obj_t *parent)
{
    lv_obj_t *object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_size(object, 640, 64);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(object, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(object, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return object;
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, bool small)
{
    lv_obj_t *object = lv_label_create(parent);
    lv_obj_set_width(object, 640);
    lv_label_set_long_mode(object, LV_LABEL_LONG_WRAP);
    if (small) lv_obj_set_style_text_font(object, &lv_font_montserrat_14, 0);
    lv_label_set_text(object, text);
    return object;
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, int width, lv_event_cb_t callback)
{
    lv_obj_t *object = lv_button_create(parent);
    lv_obj_set_size(object, width, 60);
    lv_obj_t *caption = lv_label_create(object);
    lv_label_set_text(caption, text);
    lv_obj_center(caption);
    lv_obj_add_event_cb(object, callback, LV_EVENT_CLICKED, NULL);
    return caption;
}

void subnet_tool_stop(void)
{
    if (keyboard) lv_keyboard_set_textarea(keyboard, NULL);
    for (unsigned i = 0; i < 3; i++) fields[i] = NULL;
    keyboard = NULL;
    keys_label = NULL;
    result_label = NULL;
    peer_label = NULL;
    note_label = NULL;
    updating = false;
}

void subnet_tool_show(lv_obj_t *parent)
{
    subnet_tool_stop();
    lv_obj_set_style_pad_row(parent, 12, 0);
    lv_obj_t *column = lv_obj_create(parent);
    lv_obj_remove_style_all(column);
    lv_obj_set_size(column, 640, LV_SIZE_CONTENT);
    lv_obj_remove_flag(column, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(column, 12, 0);
    lv_obj_set_style_text_font(column, &lv_font_montserrat_28, 0);
    label(column, "Subnet Lab", false);
    label(column, "IPv4 subnet calculator. Use a prefix (24 or /24) or netmask\n"
                  "(255.255.255.0). Peer comparison is optional.", true);
    static const char *const names[] = {"IPv4 address", "Prefix / netmask", "Peer (optional)"};
    for (unsigned i = 0; i < 3; i++) {
        lv_obj_t *field_row = row(column);
        lv_obj_t *caption = lv_label_create(field_row);
        lv_obj_set_width(caption, 270);
        lv_label_set_text(caption, names[i]);
        fields[i] = lv_textarea_create(field_row);
        lv_obj_set_size(fields[i], 354, 64);
        /* Preserve pasted newlines; the address/mask parser rejects them. */
        lv_textarea_set_max_length(fields[i], SUBNET_INPUT_MAX);
        lv_textarea_set_text(fields[i], inputs[i]);
        lv_obj_add_event_cb(fields[i], field_event, LV_EVENT_ALL, (void *)(uintptr_t)i);
    }
    lv_obj_t *actions = row(column);
    button(actions, "Calculate", 238, calculate_clicked);
    button(actions, "Example", 188, example_clicked);
    keys_label = button(actions, "Show keys", 188, keys_clicked);
    keyboard = lv_keyboard_create(column);
    lv_obj_set_size(keyboard, 640, 246);
    lv_obj_set_style_text_font(keyboard, &lv_font_montserrat_28, 0);
    lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_USER_1, key_map, key_controls);
    lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_USER_1);
    lv_keyboard_set_textarea(keyboard, fields[0]);
    lv_obj_add_event_cb(keyboard, keyboard_done, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(keyboard, keyboard_done, LV_EVENT_CANCEL, NULL);
    set_keyboard_visible(false);
    result_label = label(column, "", false);
    peer_label = label(column, "", false);
    note_label = label(column, "", true);
    label(column, "Offline math only. Inputs stay in RAM until restart. Host slots do not\n"
                  "exclude special-use addresses or confirm availability. Membership uses\n"
                  "this mask only; routing, VLANs and the peer's settings still matter.", true);
    calculate();
}
