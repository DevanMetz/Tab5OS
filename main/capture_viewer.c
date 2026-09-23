#include "capture_viewer.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "capture_data.h"
#include "esp_heap_caps.h"

#define VIEWER_MAX_ROWS 2048
#define VIEWER_CHART_POINTS 300

static capture_point_t *rows;
static capture_data_t data;
static lv_obj_t *chart;
static lv_chart_series_t *series;
static lv_chart_cursor_t *cursor;
static lv_obj_t *pan_slider;
static lv_obj_t *zoom_label;
static lv_obj_t *range_label;
static lv_obj_t *stats_label;
static lv_obj_t *cursor_label;
static unsigned zoom_level;
static size_t visible_start;
static size_t visible_count;

void capture_viewer_stop(void)
{
    if (rows) heap_caps_free(rows);
    rows = NULL;
    memset(&data, 0, sizeof(data));
    chart = NULL;
    series = NULL;
    cursor = NULL;
    pan_slider = NULL;
    zoom_label = NULL;
    range_label = NULL;
    stats_label = NULL;
    cursor_label = NULL;
    zoom_level = 0;
    visible_start = 0;
    visible_count = 0;
}

static size_t row_for_chart_point(uint32_t chart_point)
{
    if (visible_count <= 1) return visible_start;
    return visible_start + (size_t)chart_point * (visible_count - 1) /
                           (VIEWER_CHART_POINTS - 1);
}

static void format_time(char *text, size_t size, uint64_t value)
{
    if (data.format == CAPTURE_FORMAT_SCOPE) {
        snprintf(text, size, "%llu.%03llu ms", (unsigned long long)(value / 1000),
                 (unsigned long long)(value % 1000));
    } else {
        time_t stamp = (time_t)value;
        struct tm local;
        if (localtime_r(&stamp, &local) && strftime(text, size, "%H:%M:%S", &local)) return;
        snprintf(text, size, "%llu s", (unsigned long long)value);
    }
}

static void render_chart(void)
{
    if (!chart || !data.count) return;
    visible_count = data.count >> zoom_level;
    if (visible_count < 2) visible_count = data.count < 2 ? data.count : 2;
    size_t max_start = data.count - visible_count;
    visible_start = (size_t)lv_slider_get_value(pan_slider) * max_start / 1000;

    int32_t minimum = INT32_MAX, maximum = INT32_MIN;
    int64_t total = 0;
    size_t valid = 0;
    for (size_t i = visible_start; i < visible_start + visible_count; i++) {
        if (!rows[i].valid) continue;
        if (rows[i].value < minimum) minimum = rows[i].value;
        if (rows[i].value > maximum) maximum = rows[i].value;
        total += rows[i].value;
        valid++;
    }
    int32_t actual_minimum = minimum, actual_maximum = maximum;
    if (!valid) {
        minimum = 0;
        maximum = data.format == CAPTURE_FORMAT_I2C ? 255 : 3300;
    } else {
        int32_t padding = (maximum - minimum) / 10;
        if (padding < 1) padding = 1;
        minimum = minimum > INT32_MIN + padding ? minimum - padding : INT32_MIN;
        maximum = maximum < INT32_MAX - padding ? maximum + padding : INT32_MAX;
    }
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, minimum, maximum);
    int32_t *values = lv_chart_get_series_y_array(chart, series);
    for (uint32_t i = 0; i < VIEWER_CHART_POINTS; i++) {
        size_t row = row_for_chart_point(i);
        values[i] = rows[row].valid ? rows[row].value : LV_CHART_POINT_NONE;
    }
    lv_chart_refresh(chart);

    char first[32], last[32];
    format_time(first, sizeof(first), rows[visible_start].time);
    format_time(last, sizeof(last), rows[visible_start + visible_count - 1].time);
    lv_label_set_text_fmt(range_label, "%s to %s  |  Y: %ld to %ld %s", first, last,
                          (long)minimum, (long)maximum,
                          data.format == CAPTURE_FORMAT_SCOPE ? "mV" : "decimal");
    if (valid) {
        lv_label_set_text_fmt(stats_label, "Visible: %u/%u readings  |  Min %ld  Max %ld  Avg %ld %s",
                              (unsigned)valid, (unsigned)visible_count,
                              (long)actual_minimum, (long)actual_maximum,
                              (long)(total / (int64_t)valid),
                              data.format == CAPTURE_FORMAT_SCOPE ? "mV" : "decimal");
    } else {
        lv_label_set_text(stats_label, "No successful readings in this window");
    }
    lv_label_set_text(cursor_label, "Tap the graph to inspect a reading");
    lv_chart_set_cursor_point(chart, cursor, series, LV_CHART_POINT_NONE);
}

static void pan_changed(lv_event_t *event)
{
    (void)event;
    render_chart();
}

static void zoom_clicked(lv_event_t *event)
{
    (void)event;
    zoom_level = (zoom_level + 1) % 4;
    lv_label_set_text_fmt(zoom_label, "Zoom %ux", 1U << zoom_level);
    if (zoom_level) lv_obj_remove_state(pan_slider, LV_STATE_DISABLED);
    else lv_obj_add_state(pan_slider, LV_STATE_DISABLED);
    lv_slider_set_value(pan_slider, 0, LV_ANIM_OFF);
    render_chart();
}

static void chart_pressed(lv_event_t *event)
{
    (void)event;
    uint32_t point = lv_chart_get_pressed_point(chart);
    if (point >= VIEWER_CHART_POINTS) return;
    size_t row = row_for_chart_point(point);
    char when[32];
    format_time(when, sizeof(when), rows[row].time);
    if (rows[row].valid && data.format == CAPTURE_FORMAT_SCOPE)
        lv_label_set_text_fmt(cursor_label, "%s  |  %ld mV", when, (long)rows[row].value);
    else if (rows[row].valid)
        lv_label_set_text_fmt(cursor_label, "%s  |  0x%02lX (%ld)", when,
                              (unsigned long)rows[row].value, (long)rows[row].value);
    else
        lv_label_set_text_fmt(cursor_label, "%s  |  read failed", when);
    lv_chart_set_cursor_point(chart, cursor, series, point);
}

bool capture_viewer_show(lv_obj_t *parent, const char *path)
{
    const char *extension = strrchr(path, '.');
    if (!extension || strcasecmp(extension, ".CSV")) return false;
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    char header[256];
    if (!fgets(header, sizeof(header), file)) {
        fclose(file);
        return false;
    }
    header[strcspn(header, "\r\n")] = '\0';
    capture_format_t format = capture_data_detect(header);
    if (format == CAPTURE_FORMAT_NONE) {
        fclose(file);
        return false;
    }

    rows = heap_caps_malloc(VIEWER_MAX_ROWS * sizeof(*rows), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool loaded = rows && capture_data_read(file, rows, VIEWER_MAX_ROWS, &data);
    fclose(file);

    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    lv_obj_t *title = lv_label_create(parent);
    lv_obj_set_width(title, 640);
    lv_label_set_text_fmt(title, "%s  |  %s", format == CAPTURE_FORMAT_SCOPE ? "Scope" : "I2C watch", name);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    if (!loaded || !data.count) {
        lv_obj_t *message = lv_label_create(parent);
        lv_obj_set_width(message, 640);
        lv_label_set_text(message, !rows ? "Not enough memory to open capture" :
                          !loaded ? "Could not read capture" : "No readable capture rows in this file");
        return true;
    }

    lv_obj_t *details = lv_label_create(parent);
    lv_obj_set_width(details, 640);
    lv_label_set_text_fmt(details, "%u rows%s  |  %u malformed skipped", (unsigned)data.count,
                          data.truncated ? "  |  showing first 2048" : "", (unsigned)data.ignored);

    chart = lv_chart_create(parent);
    lv_obj_set_size(chart, 640, 480);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, VIEWER_CHART_POINTS);
    lv_chart_set_div_line_count(chart, 5, 7);
    series = lv_chart_add_series(chart, lv_palette_main(LV_PALETTE_TEAL), LV_CHART_AXIS_PRIMARY_Y);
    cursor = lv_chart_add_cursor(chart, lv_palette_main(LV_PALETTE_AMBER), LV_DIR_VER);
    lv_obj_add_event_cb(chart, chart_pressed, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *zoom = lv_button_create(parent);
    lv_obj_set_size(zoom, 240, 68);
    zoom_label = lv_label_create(zoom);
    lv_label_set_text(zoom_label, "Zoom 1x");
    lv_obj_center(zoom_label);
    lv_obj_add_event_cb(zoom, zoom_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *pan_text = lv_label_create(parent);
    lv_label_set_text(pan_text, "Pan through the recording");
    pan_slider = lv_slider_create(parent);
    lv_obj_set_width(pan_slider, 580);
    lv_slider_set_range(pan_slider, 0, 1000);
    lv_slider_set_value(pan_slider, 0, LV_ANIM_OFF);
    lv_obj_add_state(pan_slider, LV_STATE_DISABLED);
    lv_obj_add_event_cb(pan_slider, pan_changed, LV_EVENT_VALUE_CHANGED, NULL);

    range_label = lv_label_create(parent);
    lv_obj_set_width(range_label, 640);
    stats_label = lv_label_create(parent);
    lv_obj_set_width(stats_label, 640);
    cursor_label = lv_label_create(parent);
    lv_obj_set_width(cursor_label, 640);
    render_chart();
    return true;
}
