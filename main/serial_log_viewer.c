#include "serial_log_viewer.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "serial_log_data.h"
#include "payload_clipboard.h"

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
static lv_obj_t *row_select;
static lv_obj_t *copy_button;
static lv_obj_t *selection_label;
static size_t page_rows[VIEWER_PAGE_ROWS];
static size_t page_row_count;
static uint32_t selected_in_page;
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
    row_select = copy_button = selection_label = NULL;
    page_row_count = 0;
    selected_in_page = 0;
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

static void render_row(const serial_log_row_t *row, size_t index, size_t *used)
{
    char when[32];
    format_time(when, sizeof(when), row->unix_time);
    append(used, "Record %u | %s  %s  %u bytes\n", (unsigned)(index + 1), when, row->tx ? "TX" : "RX",
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

static const serial_log_row_t *selected_row(size_t *index)
{
    if (!rows || !row_select || !index) return NULL;
    uint32_t selected = lv_dropdown_get_selected(row_select);
    if (selected >= page_row_count) return NULL;
    *index = page_rows[selected];
    if (*index >= data.count || !matches(&rows[*index])) return NULL;
    return &rows[*index];
}

static void render_selection(void)
{
    if (!selection_label || !copy_button) return;
    size_t index;
    const serial_log_row_t *row = selected_row(&index);
    bool can_copy = row && row->length && row->length <= PAYLOAD_CLIPBOARD_MAX_BYTES;
    if (can_copy) lv_obj_remove_state(copy_button, LV_STATE_DISABLED);
    else lv_obj_add_state(copy_button, LV_STATE_DISABLED);
    if (!row) {
        lv_label_set_text(selection_label, "No record selected. The previous clipboard is unchanged.");
        return;
    }
    char text[200];
    snprintf(text, sizeof(text), "Selected record %u | %s | unix %llu | %u bytes\n%s",
        (unsigned)(index + 1), row->tx ? "TX" : "RX", (unsigned long long)row->unix_time,
        (unsigned)row->length, can_copy ? "COPY ROW uses the original bytes, including hidden/control bytes." :
        "This row exceeds the 128-byte clipboard. No partial copy is available.");
    lv_label_set_text(selection_label, text);
}

static void row_changed(lv_event_t *event)
{
    (void)event;
    selected_in_page = lv_dropdown_get_selected(row_select);
    render_selection();
}

static void copy_clicked(lv_event_t *event)
{
    (void)event;
    size_t index;
    const serial_log_row_t *row = selected_row(&index);
    /* Check the mapped record again; never copy the rendered/truncated text. */
    if (!row || !row->length || row->length > PAYLOAD_CLIPBOARD_MAX_BYTES) {
        render_selection();
        return;
    }
    if (payload_clipboard_store(row->bytes, row->length)) {
        lv_label_set_text_fmt(selection_label, "Copied record %u | %s | %u bytes.\n"
            "Paste into Byte Lab or RTU Frames. A log row is not necessarily a whole reply.",
            (unsigned)(index + 1), row->tx ? "TX" : "RX", (unsigned)row->length);
    } else lv_label_set_text(selection_label, "Could not copy this row. The previous clipboard is unchanged.");
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
    page_row_count = 0;
    char options[VIEWER_PAGE_ROWS * 40] = "";
    size_t option_length = 0;
    size_t found = 0;
    for (size_t i = 0; i < data.count && found < last; i++) {
        if (!matches(&rows[i])) continue;
        if (found >= first) {
            render_row(&rows[i], i, &used);
            int length = snprintf(options + option_length, sizeof(options) - option_length,
                "%s#%u %s (%u bytes)", page_row_count ? "\n" : "", (unsigned)(i + 1),
                rows[i].tx ? "TX" : "RX", (unsigned)rows[i].length);
            if (length > 0 && (size_t)length < sizeof(options) - option_length) option_length += (size_t)length;
            page_rows[page_row_count++] = i;
        }
        found++;
    }
    if (!matched) append(&used, "No rows match this filter.");
    lv_label_set_text(body_label, page_text);
    lv_obj_scroll_to_y(body_scroll, 0, LV_ANIM_OFF);
    lv_dropdown_set_options(row_select, page_row_count ? options : "No matching rows");
    if (selected_in_page >= page_row_count) selected_in_page = 0;
    lv_dropdown_set_selected(row_select, selected_in_page);
    if (page_row_count) lv_obj_remove_state(row_select, LV_STATE_DISABLED);
    else lv_obj_add_state(row_select, LV_STATE_DISABLED);
    render_selection();
}

static void filter_clicked(lv_event_t *event)
{
    (void)event;
    direction_filter = (direction_filter + 1) % 3;
    page_index = 0;
    selected_in_page = 0;
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
    selected_in_page = 0;
    render_page();
}

static void next_clicked(lv_event_t *event)
{
    (void)event;
    page_index++;
    selected_in_page = 0;
    render_page();
}

static lv_obj_t *control(lv_obj_t *parent, const char *label,
                         lv_event_cb_t callback, lv_obj_t **text)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, 285, 68);
    *text = lv_label_create(button);
    lv_label_set_text(*text, label);
    lv_obj_set_style_text_font(*text, &lv_font_montserrat_28, 0);
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
    if (!path) return false;
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
    if (fclose(file) != 0) loaded = false;

    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    const char *kind = strstr(path, "/RS485/") ? "RS-485" :
                       strstr(path, "/UART/") ? "UART" : "Serial";
    lv_obj_t *title = lv_label_create(parent);
    lv_obj_set_width(title, 640);
    lv_label_set_text_fmt(title, "%s log  |  %s", kind, name);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    if (!loaded || !data.count) {
        const char *error = !rows || !page_text ? "Not enough memory to open log" :
                            !loaded ? "Could not read log" : "No readable log rows in this file";
        serial_log_viewer_stop();
        lv_obj_t *message = lv_label_create(parent);
        lv_obj_set_width(message, 640);
        lv_label_set_text(message, error);
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

    lv_obj_t *copy_controls = control_row(parent);
    row_select = lv_dropdown_create(copy_controls);
    lv_obj_set_size(row_select, 430, 68);
    lv_obj_set_style_text_font(row_select, &lv_font_montserrat_28, 0);
    lv_obj_t *list = lv_dropdown_get_list(row_select);
    lv_obj_set_style_text_font(list, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_font(list, &lv_font_montserrat_28, LV_PART_SELECTED);
    lv_obj_set_style_text_line_space(list, 20, LV_PART_MAIN);
    lv_obj_set_style_max_height(list, 260, LV_PART_MAIN);
    lv_obj_add_event_cb(row_select, row_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t *copy_text;
    copy_button = control(copy_controls, "COPY ROW", copy_clicked, &copy_text);
    lv_obj_set_width(copy_button, 198);
    selection_label = lv_label_create(parent);
    lv_obj_set_size(selection_label, 640, 48);
    lv_obj_set_style_text_font(selection_label, &lv_font_montserrat_14, 0);

    body_scroll = lv_obj_create(parent);
    lv_obj_set_size(body_scroll, 640, 510);
    lv_obj_set_scroll_dir(body_scroll, LV_DIR_VER);
    body_label = lv_label_create(body_scroll);
    lv_obj_set_width(body_label, LV_PCT(100));
    lv_label_set_long_mode(body_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(body_label, &lv_font_montserrat_14, 0);
    lv_obj_t *note = lv_label_create(parent);
    lv_obj_set_width(note, 640);
    lv_obj_set_style_text_font(note, &lv_font_montserrat_14, 0);
    lv_label_set_text(note, "Record numbers count loaded valid rows, not CSV line numbers.\n"
                           "Copy one row of up to 128 bytes. Log chunks may split or combine protocol frames.");
    render_page();
    return true;
}
