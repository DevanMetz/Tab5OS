#include "serial_log_viewer.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "serial_log_data.h"

#define VIEWER_MAX_ROWS 512
#define VIEWER_PAGE_ROWS 8
#define VIEWER_TEXT_BYTES 8192

static serial_log_row_t *rows;
static serial_log_data_t data;
static char *page_text;
static lv_obj_t *filter_label;
static lv_obj_t *mode_label;
static lv_obj_t *summary_label;
static lv_obj_t *body_label;
static lv_obj_t *body_scroll;
static lv_obj_t *previous_button;
static lv_obj_t *next_button;
static unsigned direction_filter;
static bool ascii_view;
static size_t page_index;

void serial_log_viewer_stop(void)
{
    if (rows) heap_caps_free(rows);
    if (page_text) heap_caps_free(page_text);
    rows = NULL;
    page_text = NULL;
    memset(&data, 0, sizeof(data));
    filter_label = NULL;
    mode_label = NULL;
    summary_label = NULL;
    body_label = NULL;
    body_scroll = NULL;
    previous_button = NULL;
    next_button = NULL;
    direction_filter = 0;
    ascii_view = false;
    page_index = 0;
}

static void append(size_t *used, const char *format, ...)
{
    if (*used >= VIEWER_TEXT_BYTES - 1) return;
    va_list arguments;
    va_start(arguments, format);
    int written = vsnprintf(page_text + *used, VIEWER_TEXT_BYTES - *used, format, arguments);
    va_end(arguments);
    if (written < 0) return;
    size_t available = VIEWER_TEXT_BYTES - *used;
    *used += (size_t)written < available ? (size_t)written : available - 1;
}

static bool matches(const serial_log_row_t *row)
{
    return direction_filter == 0 || (direction_filter == 1 && !row->tx) ||
           (direction_filter == 2 && row->tx);
}

static void format_time(char *text, size_t capacity, uint64_t value)
{
    time_t stamp = (time_t)value;
    struct tm local;
    if (localtime_r(&stamp, &local) &&
        strftime(text, capacity, "%m-%d %H:%M:%S", &local)) return;
    snprintf(text, capacity, "%llu", (unsigned long long)value);
}

static void render_row(const serial_log_row_t *row, size_t *used)
{
    char when[32];
    format_time(when, sizeof(when), row->unix_time);
    append(used, "%s  %s  %u bytes\n", when, row->tx ? "TX" : "RX",
           (unsigned)row->length);
    if (ascii_view) {
        for (uint16_t i = 0; i < row->length; i++) {
            uint8_t byte = row->bytes[i];
            if (byte == '\r') append(used, "\\r");
            else if (byte == '\n') append(used, "\\n");
            else if (byte == '\t') append(used, "\\t");
            else append(used, "%c", byte >= 32 && byte <= 126 ? byte : '.');
        }
        append(used, "\n\n");
    } else {
        for (uint16_t i = 0; i < row->length; i++)
            append(used, "%02X%s", row->bytes[i],
                   (i + 1) % 16 == 0 || i + 1 == row->length ? "\n" : " ");
        append(used, "\n");
    }
}

static void render_page(void)
{
    if (!body_label || !page_text) return;
    size_t matched = 0;
    for (size_t i = 0; i < data.count; i++)
        if (matches(&rows[i])) matched++;
    if (page_index && page_index * VIEWER_PAGE_ROWS >= matched)
        page_index = matched ? (matched - 1) / VIEWER_PAGE_ROWS : 0;

    static const char *filters[] = {"All", "RX", "TX"};
    lv_label_set_text_fmt(filter_label, "Filter: %s", filters[direction_filter]);
    lv_label_set_text(mode_label, ascii_view ? "View: ASCII" : "View: Hex");
    size_t first = page_index * VIEWER_PAGE_ROWS;
    size_t last = first + VIEWER_PAGE_ROWS;
    if (last > matched) last = matched;
    lv_label_set_text_fmt(summary_label,
                          "Rows %u-%u of %u  |  %u malformed skipped%s",
                          matched ? (unsigned)(first + 1) : 0, (unsigned)last,
                          (unsigned)matched, (unsigned)data.ignored,
                          data.truncated ? "  |  first 512 loaded" : "");
    if (first) lv_obj_remove_state(previous_button, LV_STATE_DISABLED);
    else lv_obj_add_state(previous_button, LV_STATE_DISABLED);
    if (last < matched) lv_obj_remove_state(next_button, LV_STATE_DISABLED);
    else lv_obj_add_state(next_button, LV_STATE_DISABLED);

    size_t used = 0;
    page_text[0] = '\0';
    size_t found = 0;
    for (size_t i = 0; i < data.count && found < last; i++) {
        if (!matches(&rows[i])) continue;
        if (found >= first) render_row(&rows[i], &used);
        found++;
    }
    if (!matched) append(&used, "No rows match this filter.");
    lv_label_set_text(body_label, page_text);
    lv_obj_scroll_to_y(body_scroll, 0, LV_ANIM_OFF);
}

static void filter_clicked(lv_event_t *event)
{
    (void)event;
    direction_filter = (direction_filter + 1) % 3;
    page_index = 0;
    render_page();
}

static void mode_clicked(lv_event_t *event)
{
    (void)event;
    ascii_view = !ascii_view;
    render_page();
}

static void previous_clicked(lv_event_t *event)
{
    (void)event;
    if (page_index) page_index--;
    render_page();
}

static void next_clicked(lv_event_t *event)
{
    (void)event;
    page_index++;
    render_page();
}

static lv_obj_t *control(lv_obj_t *parent, const char *label,
                         lv_event_cb_t callback, lv_obj_t **text)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, 285, 68);
    *text = lv_label_create(button);
    lv_label_set_text(*text, label);
    lv_obj_center(*text);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, NULL);
    return button;
}

static lv_obj_t *control_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 640, 76);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return row;
}

bool serial_log_viewer_show(lv_obj_t *parent, const char *path)
{
    const char *extension = strrchr(path, '.');
    if (!extension || strcasecmp(extension, ".CSV")) return false;
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    char header[64];
    if (!fgets(header, sizeof(header), file)) {
        fclose(file);
        return false;
    }
    header[strcspn(header, "\r\n")] = '\0';
    if (!serial_log_detect(header)) {
        fclose(file);
        return false;
    }

    rows = heap_caps_malloc(VIEWER_MAX_ROWS * sizeof(*rows),
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    page_text = heap_caps_malloc(VIEWER_TEXT_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool loaded = rows && page_text && serial_log_read(file, rows, VIEWER_MAX_ROWS, &data);
    fclose(file);

    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    const char *kind = strstr(path, "/RS485/") ? "RS-485" :
                       strstr(path, "/UART/") ? "UART" : "Serial";
    lv_obj_t *title = lv_label_create(parent);
    lv_obj_set_width(title, 640);
    lv_label_set_text_fmt(title, "%s log  |  %s", kind, name);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    if (!loaded || !data.count) {
        lv_obj_t *message = lv_label_create(parent);
        lv_obj_set_width(message, 640);
        lv_label_set_text(message, !rows || !page_text ? "Not enough memory to open log" :
                          !loaded ? "Could not read log" : "No readable log rows in this file");
        return true;
    }

    lv_obj_t *view_controls = control_row(parent);
    control(view_controls, "Filter: All", filter_clicked, &filter_label);
    control(view_controls, "View: Hex", mode_clicked, &mode_label);

    lv_obj_t *page_controls = control_row(parent);
    lv_obj_t *previous_text;
    lv_obj_t *next_text;
    previous_button = control(page_controls, "Previous", previous_clicked, &previous_text);
    next_button = control(page_controls, "Next", next_clicked, &next_text);

    summary_label = lv_label_create(parent);
    lv_obj_set_width(summary_label, 640);
    lv_obj_set_style_text_font(summary_label, &lv_font_montserrat_14, 0);

    body_scroll = lv_obj_create(parent);
    lv_obj_set_size(body_scroll, 640, 690);
    lv_obj_set_scroll_dir(body_scroll, LV_DIR_VER);
    body_label = lv_label_create(body_scroll);
    lv_obj_set_width(body_label, 600);
    lv_label_set_long_mode(body_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(body_label, &lv_font_montserrat_14, 0);
    render_page();
    return true;
}
