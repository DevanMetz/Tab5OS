#include "ble_tool.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs_adv.h"
#include "host/util/util.h"
#include "os/os_mbuf.h"
#include "storage_io.h"

#define BLE_TOOL_DEVICE_MAX 8
#define BLE_TOOL_SERVICE_MAX 16
#define BLE_TOOL_CHARACTERISTIC_MAX 32
#define BLE_TOOL_VALUE_MAX 64
#define BLE_TOOL_WRITE_MAX 64
#define BLE_TOOL_CONFIRM_MS 5000U

typedef struct {
    ble_addr_t address;
    int8_t rssi;
    char name[33];
} ble_tool_device_t;

typedef struct {
    uint16_t start_handle;
    uint16_t end_handle;
    ble_uuid_any_t uuid;
} ble_tool_service_t;

typedef struct {
    uint16_t def_handle;
    uint16_t val_handle;
    uint8_t properties;
    uint8_t service_index;
    ble_uuid_any_t uuid;
} ble_tool_characteristic_t;

static const char *TAG = "ble-tool";
static portMUX_TYPE tool_lock = portMUX_INITIALIZER_UNLOCKED;
static lv_obj_t *screen_parent;
static lv_obj_t *status_label;
static lv_obj_t *value_label;
static lv_obj_t *write_area;
static lv_timer_t *ui_timer;
static ble_tool_scan_refresh_cb_t refresh_scan;
static ble_tool_connect_allowed_cb_t connection_allowed;
static ble_tool_storage_error_cb_t report_storage_error;

static bool tool_available;
static bool screen_sd_available;
static bool screen_active;
static bool scan_requested;
static bool connecting;
static bool connected;
static bool stopping;
static bool discovering;
static bool operation_busy;
static bool view_dirty;
static bool status_dirty;
static bool value_dirty;
static bool services_truncated;
static bool characteristics_truncated;
static uint16_t connection_handle = BLE_HS_CONN_HANDLE_NONE;
static ble_tool_device_t devices[BLE_TOOL_DEVICE_MAX];
static size_t device_count;
static int selected_device = -1;
static ble_tool_service_t services[BLE_TOOL_SERVICE_MAX];
static size_t service_count;
static size_t service_cursor;
static ble_tool_characteristic_t characteristics[BLE_TOOL_CHARACTERISTIC_MAX];
static size_t characteristic_count;
static int selected_characteristic = -1;
static char status_text[192] = "Ready; scanning and connections are opt-in";

static uint8_t last_value[BLE_TOOL_VALUE_MAX];
static size_t last_value_length;
static size_t last_value_total;
static uint16_t last_value_handle;
static bool last_value_seen;
static bool last_value_was_notification;

static bool write_armed;
static uint32_t write_armed_at;
static uint16_t write_armed_handle;
static uint8_t write_armed_value[BLE_TOOL_WRITE_MAX];
static size_t write_armed_length;

static uint16_t subscription_target_handle;
static uint16_t subscription_cccd_handle;
static uint16_t subscribed_value_handle;
static uint16_t subscribed_cccd_handle;
static bool subscription_enable;
static bool subscription_indicate;

static int generic_gap_event(struct ble_gap_event *event, void *argument);
static int characteristic_found(uint16_t conn_handle, const struct ble_gatt_error *error,
                                const struct ble_gatt_chr *characteristic, void *argument);

static void set_status(const char *format, ...)
{
    char text[sizeof(status_text)];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    portENTER_CRITICAL(&tool_lock);
    snprintf(status_text, sizeof(status_text), "%s", text);
    status_dirty = true;
    portEXIT_CRITICAL(&tool_lock);
}

static void request_view(void)
{
    portENTER_CRITICAL(&tool_lock);
    view_dirty = true;
    portEXIT_CRITICAL(&tool_lock);
}

static int hex_nibble(char character)
{
    if (character >= '0' && character <= '9') return character - '0';
    character = (char)tolower((unsigned char)character);
    return character >= 'a' && character <= 'f' ? character - 'a' + 10 : -1;
}

static bool parse_hex(const char *text, uint8_t *output, size_t *length)
{
    size_t count = 0;
    while (*text) {
        while (*text == ' ') text++;
        if (!*text) break;
        int high = hex_nibble(*text++);
        int low = *text ? hex_nibble(*text++) : -1;
        if (high < 0 || low < 0 || count == BLE_TOOL_WRITE_MAX) return false;
        output[count++] = (uint8_t)((high << 4) | low);
        if (*text && *text != ' ') return false;
    }
    *length = count;
    return count > 0;
}

static bool confirmation_valid(bool armed, uint16_t armed_handle, uint16_t handle,
                               const uint8_t *armed_value, size_t armed_length,
                               const uint8_t *value, size_t length,
                               uint32_t armed_at, uint32_t now)
{
    return armed && armed_handle == handle && armed_length == length &&
           memcmp(armed_value, value, length) == 0 &&
           (uint32_t)(now - armed_at) <= BLE_TOOL_CONFIRM_MS;
}

static void sanitize_name(const uint8_t *source, size_t length, char output[33])
{
    size_t copied = 0;
    for (size_t i = 0; source && i < length && copied < 32; i++)
        output[copied++] = source[i] >= 32 && source[i] <= 126 ? (char)source[i] : '?';
    while (copied && output[copied - 1] == ' ') copied--;
    output[copied] = '\0';
    if (!copied) snprintf(output, 33, "(unnamed)");
}

static bool address_equal(const ble_addr_t *left, const ble_addr_t *right)
{
    return left->type == right->type && memcmp(left->val, right->val, sizeof(left->val)) == 0;
}

static void address_text(const ble_addr_t *address, char output[18])
{
    snprintf(output, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             address->val[5], address->val[4], address->val[3],
             address->val[2], address->val[1], address->val[0]);
}

static void properties_text(uint8_t properties, char output[16])
{
    size_t length = 0;
    if (properties & BLE_GATT_CHR_PROP_READ) output[length++] = 'R';
    if (properties & BLE_GATT_CHR_PROP_WRITE) output[length++] = 'W';
    if (properties & BLE_GATT_CHR_PROP_WRITE_NO_RSP) output[length++] = 'w';
    if (properties & BLE_GATT_CHR_PROP_NOTIFY) output[length++] = 'N';
    if (properties & BLE_GATT_CHR_PROP_INDICATE) output[length++] = 'I';
    if (!length) output[length++] = '-';
    output[length] = '\0';
}

static void csv_text(const char *source, char *output, size_t capacity)
{
    size_t length = 0;
    while (source && *source && length + 1 < capacity) {
        char character = *source++;
        output[length++] = character == ',' || character == '"' || character == '\r' ||
                           character == '\n' ? '_' : character;
    }
    output[length] = '\0';
}

static void format_value(char *output, size_t capacity)
{
    uint8_t value[BLE_TOOL_VALUE_MAX];
    size_t length;
    size_t total;
    uint16_t handle;
    bool seen;
    bool was_notification;
    portENTER_CRITICAL(&tool_lock);
    length = last_value_length;
    total = last_value_total;
    handle = last_value_handle;
    seen = last_value_seen;
    was_notification = last_value_was_notification;
    memcpy(value, last_value, length);
    portEXIT_CRITICAL(&tool_lock);
    if (!seen) {
        snprintf(output, capacity, "No value read or received yet");
        return;
    }
    int used = snprintf(output, capacity, "Handle 0x%04X: %u byte%s%s", handle,
                        (unsigned)total, total == 1 ? "" : "s",
                        was_notification ? " (notification received)" : "");
    for (size_t i = 0; i < length && used > 0 && (size_t)used < capacity; i++)
        used += snprintf(output + used, capacity - (size_t)used, "%s%02X",
                         i ? " " : "\n", value[i]);
    if (total > length && used > 0 && (size_t)used < capacity)
        snprintf(output + used, capacity - (size_t)used, " ...");
}

static void store_value(uint16_t handle, struct os_mbuf *mbuf, bool notification)
{
    size_t total = mbuf ? OS_MBUF_PKTLEN(mbuf) : 0;
    size_t length = total < BLE_TOOL_VALUE_MAX ? total : BLE_TOOL_VALUE_MAX;
    uint8_t value[BLE_TOOL_VALUE_MAX];
    if (length && os_mbuf_copydata(mbuf, 0, length, value) != 0) length = 0;
    portENTER_CRITICAL(&tool_lock);
    memcpy(last_value, value, length);
    last_value_length = length;
    last_value_total = total;
    last_value_handle = handle;
    last_value_seen = true;
    last_value_was_notification = notification;
    value_dirty = true;
    portEXIT_CRITICAL(&tool_lock);
}

static bool connection_is_active(uint16_t handle)
{
    portENTER_CRITICAL(&tool_lock);
    bool active = connected && connection_handle == handle;
    portEXIT_CRITICAL(&tool_lock);
    return active;
}

static void reset_connection_data_locked(void)
{
    service_count = 0;
    service_cursor = 0;
    characteristic_count = 0;
    selected_characteristic = -1;
    services_truncated = false;
    characteristics_truncated = false;
    last_value_length = last_value_total = 0;
    last_value_handle = 0;
    last_value_seen = false;
    last_value_was_notification = false;
    write_armed = false;
    operation_busy = false;
    subscription_target_handle = subscription_cccd_handle = 0;
    subscribed_value_handle = subscribed_cccd_handle = 0;
    subscription_enable = subscription_indicate = false;
}

static void finish_discovery(void)
{
    size_t service_total;
    size_t characteristic_total;
    bool service_cap;
    bool characteristic_cap;
    portENTER_CRITICAL(&tool_lock);
    discovering = false;
    service_total = service_count;
    characteristic_total = characteristic_count;
    service_cap = services_truncated;
    characteristic_cap = characteristics_truncated;
    view_dirty = true;
    portEXIT_CRITICAL(&tool_lock);
    set_status("Discovered %u service%s and %u characteristic%s%s",
               (unsigned)service_total, service_total == 1 ? "" : "s",
               (unsigned)characteristic_total, characteristic_total == 1 ? "" : "s",
               service_cap || characteristic_cap ? "; bounded list truncated" : "");
}

static void start_next_characteristic_discovery(void)
{
    uint16_t connection;
    uint16_t start;
    uint16_t end;
    portENTER_CRITICAL(&tool_lock);
    if (!connected || service_cursor >= service_count) {
        bool done = connected;
        portEXIT_CRITICAL(&tool_lock);
        if (done) finish_discovery();
        return;
    }
    connection = connection_handle;
    start = services[service_cursor].start_handle;
    end = services[service_cursor].end_handle;
    portEXIT_CRITICAL(&tool_lock);
    int rc = ble_gattc_disc_all_chrs(connection, start, end, characteristic_found, NULL);
    if (rc) {
        portENTER_CRITICAL(&tool_lock);
        discovering = false;
        operation_busy = false;
        portEXIT_CRITICAL(&tool_lock);
        set_status("Characteristic discovery could not start: %d", rc);
        request_view();
    }
}

static int service_found(uint16_t conn_handle, const struct ble_gatt_error *error,
                         const struct ble_gatt_svc *service, void *argument)
{
    (void)argument;
    if (!connection_is_active(conn_handle)) return 0;
    if (!error->status && service) {
        portENTER_CRITICAL(&tool_lock);
        if (service_count < BLE_TOOL_SERVICE_MAX) {
            services[service_count].start_handle = service->start_handle;
            services[service_count].end_handle = service->end_handle;
            services[service_count].uuid = service->uuid;
            service_count++;
        } else services_truncated = true;
        portEXIT_CRITICAL(&tool_lock);
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
        portENTER_CRITICAL(&tool_lock);
        service_cursor = 0;
        portEXIT_CRITICAL(&tool_lock);
        start_next_characteristic_discovery();
    } else {
        portENTER_CRITICAL(&tool_lock);
        discovering = false;
        portEXIT_CRITICAL(&tool_lock);
        set_status("Service discovery failed: %u", error->status);
        request_view();
    }
    return 0;
}

static int characteristic_found(uint16_t conn_handle, const struct ble_gatt_error *error,
                                const struct ble_gatt_chr *characteristic, void *argument)
{
    (void)argument;
    if (!connection_is_active(conn_handle)) return 0;
    if (!error->status && characteristic) {
        portENTER_CRITICAL(&tool_lock);
        if (characteristic_count < BLE_TOOL_CHARACTERISTIC_MAX) {
            ble_tool_characteristic_t *target = &characteristics[characteristic_count++];
            target->def_handle = characteristic->def_handle;
            target->val_handle = characteristic->val_handle;
            target->properties = characteristic->properties;
            target->service_index = (uint8_t)service_cursor;
            target->uuid = characteristic->uuid;
        } else characteristics_truncated = true;
        portEXIT_CRITICAL(&tool_lock);
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
        portENTER_CRITICAL(&tool_lock);
        service_cursor++;
        portEXIT_CRITICAL(&tool_lock);
        start_next_characteristic_discovery();
    } else {
        portENTER_CRITICAL(&tool_lock);
        discovering = false;
        portEXIT_CRITICAL(&tool_lock);
        set_status("Characteristic discovery failed: %u", error->status);
        request_view();
    }
    return 0;
}

static int read_done(uint16_t conn_handle, const struct ble_gatt_error *error,
                     struct ble_gatt_attr *attribute, void *argument)
{
    (void)argument;
    if (!connection_is_active(conn_handle)) return 0;
    portENTER_CRITICAL(&tool_lock);
    operation_busy = false;
    portEXIT_CRITICAL(&tool_lock);
    if (!error->status && attribute) {
        size_t length = attribute->om ? OS_MBUF_PKTLEN(attribute->om) : 0;
        store_value(attribute->handle, attribute->om, false);
        set_status("Read %u byte%s from handle 0x%04X",
                   (unsigned)length, length == 1 ? "" : "s", attribute->handle);
    } else set_status("Read failed: %u", error->status);
    return 0;
}

static int write_done(uint16_t conn_handle, const struct ble_gatt_error *error,
                      struct ble_gatt_attr *attribute, void *argument)
{
    (void)attribute;
    (void)argument;
    if (!connection_is_active(conn_handle)) return 0;
    portENTER_CRITICAL(&tool_lock);
    operation_busy = false;
    portEXIT_CRITICAL(&tool_lock);
    set_status(error->status ? "Write failed: %u" : "Write acknowledged", error->status);
    return 0;
}

static int subscription_written(uint16_t conn_handle, const struct ble_gatt_error *error,
                                struct ble_gatt_attr *attribute, void *argument)
{
    (void)attribute;
    (void)argument;
    if (!connection_is_active(conn_handle)) return 0;
    bool enabled;
    portENTER_CRITICAL(&tool_lock);
    operation_busy = false;
    enabled = subscription_enable;
    if (!error->status) {
        if (enabled) {
            subscribed_value_handle = subscription_target_handle;
            subscribed_cccd_handle = subscription_cccd_handle;
        } else {
            subscribed_value_handle = 0;
            subscribed_cccd_handle = 0;
        }
    }
    view_dirty = true;
    portEXIT_CRITICAL(&tool_lock);
    set_status(error->status ? "Subscription update failed: %u" :
               enabled ? "Notifications enabled" : "Notifications disabled", error->status);
    return 0;
}

static int descriptor_found(uint16_t conn_handle, const struct ble_gatt_error *error,
                            uint16_t characteristic_handle, const struct ble_gatt_dsc *descriptor,
                            void *argument)
{
    (void)characteristic_handle;
    (void)argument;
    if (!connection_is_active(conn_handle)) return 0;
    if (!error->status && descriptor &&
        ble_uuid_cmp(&descriptor->uuid.u, BLE_UUID16_DECLARE(0x2902)) == 0) {
        portENTER_CRITICAL(&tool_lock);
        subscription_cccd_handle = descriptor->handle;
        portEXIT_CRITICAL(&tool_lock);
        return 0;
    }
    if (error->status != BLE_HS_EDONE) {
        portENTER_CRITICAL(&tool_lock);
        operation_busy = false;
        portEXIT_CRITICAL(&tool_lock);
        set_status("Descriptor discovery failed: %u", error->status);
        return 0;
    }
    uint16_t cccd;
    bool indicate;
    portENTER_CRITICAL(&tool_lock);
    cccd = subscription_cccd_handle;
    indicate = subscription_indicate;
    portEXIT_CRITICAL(&tool_lock);
    if (!cccd) {
        portENTER_CRITICAL(&tool_lock);
        operation_busy = false;
        portEXIT_CRITICAL(&tool_lock);
        set_status("Client configuration descriptor was not found");
        return 0;
    }
    const uint8_t value[] = {indicate ? 2 : 1, 0};
    int rc = ble_gattc_write_flat(conn_handle, cccd, value, sizeof(value),
                                  subscription_written, NULL);
    if (rc) {
        portENTER_CRITICAL(&tool_lock);
        operation_busy = false;
        portEXIT_CRITICAL(&tool_lock);
        set_status("Subscription could not start: %d", rc);
    }
    return 0;
}

static int generic_gap_event(struct ble_gap_event *event, void *argument)
{
    (void)argument;
    if (event->type == BLE_GAP_EVENT_CONNECT) {
        bool keep;
        portENTER_CRITICAL(&tool_lock);
        keep = screen_active && !stopping && event->connect.status == 0;
        connecting = false;
        connected = keep;
        connection_handle = event->connect.conn_handle;
        stopping = event->connect.status == 0 && !keep;
        if (!event->connect.status) reset_connection_data_locked();
        view_dirty = true;
        portEXIT_CRITICAL(&tool_lock);
        if (event->connect.status) {
            portENTER_CRITICAL(&tool_lock);
            stopping = false;
            connection_handle = BLE_HS_CONN_HANDLE_NONE;
            portEXIT_CRITICAL(&tool_lock);
            set_status("Connection failed: %d", event->connect.status);
        } else if (!keep) {
            ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        } else {
            portENTER_CRITICAL(&tool_lock);
            discovering = true;
            portEXIT_CRITICAL(&tool_lock);
            set_status("Connected; discovering services and characteristics...");
            int rc = ble_gattc_disc_all_svcs(event->connect.conn_handle, service_found, NULL);
            if (rc) {
                portENTER_CRITICAL(&tool_lock);
                discovering = false;
                portEXIT_CRITICAL(&tool_lock);
                set_status("Service discovery could not start: %d", rc);
            }
        }
        if (refresh_scan) refresh_scan();
        return 0;
    }
    if (event->type == BLE_GAP_EVENT_DISCONNECT) {
        portENTER_CRITICAL(&tool_lock);
        connecting = connected = stopping = discovering = operation_busy = false;
        connection_handle = BLE_HS_CONN_HANDLE_NONE;
        reset_connection_data_locked();
        view_dirty = true;
        portEXIT_CRITICAL(&tool_lock);
        set_status(screen_active ? "Disconnected; start a new scan when ready" : "Disconnected");
        if (refresh_scan) refresh_scan();
        return 0;
    }
    if (event->type == BLE_GAP_EVENT_NOTIFY_RX &&
        connection_is_active(event->notify_rx.conn_handle)) {
        store_value(event->notify_rx.attr_handle, event->notify_rx.om, true);
        return 0;
    }
    return 0;
}

static lv_obj_t *action_button(lv_obj_t *parent, const char *text, int width,
                               lv_event_cb_t callback, void *user_data)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, width, 72);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);
    lv_obj_t *label = lv_label_create(button);
    lv_obj_set_width(label, width - 20);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return button;
}

static lv_obj_t *row(lv_obj_t *parent, int height)
{
    lv_obj_t *container = lv_obj_create(parent);
    lv_obj_set_size(container, 640, height);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(container, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return container;
}

static void scan_clicked(lv_event_t *event)
{
    (void)event;
    if (connection_allowed && !connection_allowed()) {
        set_status("Turn Govee, Ring, and KICKR Bluetooth off before scanning");
        return;
    }
    bool start;
    portENTER_CRITICAL(&tool_lock);
    start = !scan_requested;
    scan_requested = start && tool_available && !connecting && !connected && !stopping;
    if (scan_requested) {
        device_count = 0;
        selected_device = -1;
    }
    view_dirty = true;
    portEXIT_CRITICAL(&tool_lock);
    set_status(!tool_available ? "Bluetooth hardware is unavailable" :
               start ? "Scanning; tap a device to select it" : "Scan stopped");
    if (refresh_scan) refresh_scan();
}

static void device_clicked(lv_event_t *event)
{
    int index = (int)(intptr_t)lv_event_get_user_data(event);
    portENTER_CRITICAL(&tool_lock);
    if (index >= 0 && (size_t)index < device_count) selected_device = index;
    view_dirty = true;
    portEXIT_CRITICAL(&tool_lock);
    set_status("Device selected; connection remains off until you tap Connect");
}

static void connect_clicked(lv_event_t *event)
{
    (void)event;
    if (connection_allowed && !connection_allowed()) {
        set_status("Turn Govee, Ring, and KICKR Bluetooth off before connecting");
        return;
    }
    ble_addr_t peer;
    portENTER_CRITICAL(&tool_lock);
    if (!tool_available || selected_device < 0 || (size_t)selected_device >= device_count ||
        connecting || connected || stopping) {
        portEXIT_CRITICAL(&tool_lock);
        return;
    }
    peer = devices[selected_device].address;
    scan_requested = false;
    connecting = true;
    view_dirty = true;
    portEXIT_CRITICAL(&tool_lock);
    set_status("Connecting explicitly...");
    if (refresh_scan) refresh_scan();
    if (ble_gap_disc_active()) ble_gap_disc_cancel();
    uint8_t own_address_type;
    int rc = ble_hs_id_infer_auto(0, &own_address_type);
    if (!rc) rc = ble_gap_connect(own_address_type, &peer, 30000, NULL, generic_gap_event, NULL);
    if (rc) {
        portENTER_CRITICAL(&tool_lock);
        connecting = false;
        portEXIT_CRITICAL(&tool_lock);
        set_status("Connection could not start: %d", rc);
        request_view();
        if (refresh_scan) refresh_scan();
    }
}

static void disconnect_clicked(lv_event_t *event)
{
    (void)event;
    uint16_t handle;
    portENTER_CRITICAL(&tool_lock);
    if (!connected || stopping) {
        portEXIT_CRITICAL(&tool_lock);
        return;
    }
    handle = connection_handle;
    stopping = true;
    view_dirty = true;
    portEXIT_CRITICAL(&tool_lock);
    set_status("Disconnecting...");
    int rc = ble_gap_terminate(handle, BLE_ERR_REM_USER_CONN_TERM);
    if (rc) set_status("Disconnect request failed: %d", rc);
}

static void characteristic_clicked(lv_event_t *event)
{
    int index = (int)(intptr_t)lv_event_get_user_data(event);
    portENTER_CRITICAL(&tool_lock);
    if (index >= 0 && (size_t)index < characteristic_count) selected_characteristic = index;
    write_armed = false;
    view_dirty = true;
    portEXIT_CRITICAL(&tool_lock);
    set_status("Characteristic selected; reads and subscriptions are explicit");
}

static void characteristics_back_clicked(lv_event_t *event)
{
    (void)event;
    portENTER_CRITICAL(&tool_lock);
    selected_characteristic = -1;
    write_armed = false;
    view_dirty = true;
    portEXIT_CRITICAL(&tool_lock);
}

static bool selected_characteristic_snapshot(ble_tool_characteristic_t *output,
                                             uint16_t *connection)
{
    portENTER_CRITICAL(&tool_lock);
    bool valid = connected && !stopping && selected_characteristic >= 0 &&
                 (size_t)selected_characteristic < characteristic_count;
    if (valid) {
        *output = characteristics[selected_characteristic];
        *connection = connection_handle;
    }
    portEXIT_CRITICAL(&tool_lock);
    return valid;
}

static void read_clicked(lv_event_t *event)
{
    (void)event;
    ble_tool_characteristic_t characteristic;
    uint16_t connection;
    if (!selected_characteristic_snapshot(&characteristic, &connection) ||
        !(characteristic.properties & BLE_GATT_CHR_PROP_READ)) return;
    portENTER_CRITICAL(&tool_lock);
    if (operation_busy) {
        portEXIT_CRITICAL(&tool_lock);
        return;
    }
    operation_busy = true;
    portEXIT_CRITICAL(&tool_lock);
    set_status("Reading handle 0x%04X...", characteristic.val_handle);
    int rc = ble_gattc_read(connection, characteristic.val_handle, read_done, NULL);
    if (rc) {
        portENTER_CRITICAL(&tool_lock);
        operation_busy = false;
        portEXIT_CRITICAL(&tool_lock);
        set_status("Read could not start: %d", rc);
    }
}

static void write_clicked(lv_event_t *event)
{
    (void)event;
    ble_tool_characteristic_t characteristic;
    uint16_t connection;
    if (!selected_characteristic_snapshot(&characteristic, &connection)) return;
    uint8_t value[BLE_TOOL_WRITE_MAX];
    size_t length;
    if (!write_area || !parse_hex(lv_textarea_get_text(write_area), value, &length)) {
        set_status("Enter 1-64 bytes as two hex digits each, separated by spaces");
        return;
    }
    bool writable = characteristic.properties &
                    (BLE_GATT_CHR_PROP_WRITE | BLE_GATT_CHR_PROP_WRITE_NO_RSP);
    if (!writable) return;
    uint32_t now = lv_tick_get();
    portENTER_CRITICAL(&tool_lock);
    bool confirmed = confirmation_valid(write_armed, write_armed_handle,
                                        characteristic.val_handle, write_armed_value,
                                        write_armed_length, value, length,
                                        write_armed_at, now);
    if (!confirmed) {
        write_armed = true;
        write_armed_at = now;
        write_armed_handle = characteristic.val_handle;
        write_armed_length = length;
        memcpy(write_armed_value, value, length);
        portEXIT_CRITICAL(&tool_lock);
        set_status("Armed: write %u byte%s to handle 0x%04X. Tap again unchanged within 5s.",
                   (unsigned)length, length == 1 ? "" : "s", characteristic.val_handle);
        return;
    }
    if (operation_busy) {
        portEXIT_CRITICAL(&tool_lock);
        return;
    }
    write_armed = false;
    operation_busy = true;
    portEXIT_CRITICAL(&tool_lock);
    int rc;
    if (characteristic.properties & BLE_GATT_CHR_PROP_WRITE)
        rc = ble_gattc_write_flat(connection, characteristic.val_handle, value, length,
                                  write_done, NULL);
    else {
        rc = ble_gattc_write_no_rsp_flat(connection, characteristic.val_handle, value, length);
        portENTER_CRITICAL(&tool_lock);
        operation_busy = false;
        portEXIT_CRITICAL(&tool_lock);
        if (!rc) set_status("Write without response queued");
    }
    if (rc) {
        portENTER_CRITICAL(&tool_lock);
        operation_busy = false;
        portEXIT_CRITICAL(&tool_lock);
        set_status("Write could not start: %d", rc);
    }
}

static void subscribe_clicked(lv_event_t *event)
{
    (void)event;
    ble_tool_characteristic_t characteristic;
    uint16_t connection;
    if (!selected_characteristic_snapshot(&characteristic, &connection)) return;
    bool can_subscribe = characteristic.properties &
                         (BLE_GATT_CHR_PROP_NOTIFY | BLE_GATT_CHR_PROP_INDICATE);
    if (!can_subscribe) return;
    portENTER_CRITICAL(&tool_lock);
    if (operation_busy) {
        portEXIT_CRITICAL(&tool_lock);
        return;
    }
    if (subscribed_value_handle && subscribed_value_handle != characteristic.val_handle) {
        portEXIT_CRITICAL(&tool_lock);
        set_status("Disable the current subscription before selecting another");
        return;
    }
    operation_busy = true;
    if (subscribed_value_handle == characteristic.val_handle) {
        subscription_enable = false;
        uint16_t cccd = subscribed_cccd_handle;
        portEXIT_CRITICAL(&tool_lock);
        const uint8_t disabled[] = {0, 0};
        int rc = ble_gattc_write_flat(connection, cccd, disabled, sizeof(disabled),
                                      subscription_written, NULL);
        if (rc) {
            portENTER_CRITICAL(&tool_lock);
            operation_busy = false;
            portEXIT_CRITICAL(&tool_lock);
            set_status("Unsubscribe could not start: %d", rc);
        } else set_status("Disabling notifications...");
        return;
    }
    subscription_enable = true;
    subscription_target_handle = characteristic.val_handle;
    subscription_cccd_handle = 0;
    subscription_indicate = !(characteristic.properties & BLE_GATT_CHR_PROP_NOTIFY);
    uint16_t end = services[characteristic.service_index].end_handle;
    for (size_t i = 0; i < characteristic_count; i++)
        if (characteristics[i].service_index == characteristic.service_index &&
            characteristics[i].def_handle > characteristic.def_handle &&
            characteristics[i].def_handle - 1 < end)
            end = characteristics[i].def_handle - 1;
    portEXIT_CRITICAL(&tool_lock);
    set_status("Finding the notification descriptor...");
    int rc = ble_gattc_disc_all_dscs(connection, characteristic.val_handle, end,
                                     descriptor_found, NULL);
    if (rc) {
        portENTER_CRITICAL(&tool_lock);
        operation_busy = false;
        portEXIT_CRITICAL(&tool_lock);
        set_status("Descriptor discovery could not start: %d", rc);
    }
}

static void save_evidence_clicked(lv_event_t *event)
{
    (void)event;
    ble_tool_device_t device_snapshot[BLE_TOOL_DEVICE_MAX];
    ble_tool_service_t service_snapshot[BLE_TOOL_SERVICE_MAX];
    ble_tool_characteristic_t characteristic_snapshot[BLE_TOOL_CHARACTERISTIC_MAX];
    uint8_t value_snapshot[BLE_TOOL_VALUE_MAX];
    size_t devices_found;
    size_t services_found;
    size_t characteristics_found;
    size_t value_length;
    size_t value_total;
    uint16_t value_handle;
    bool value_seen;
    bool value_was_notification;
    int peer_index;
    portENTER_CRITICAL(&tool_lock);
    bool ready = connected && !stopping && !discovering && screen_sd_available;
    devices_found = device_count;
    services_found = service_count;
    characteristics_found = characteristic_count;
    memcpy(device_snapshot, devices, devices_found * sizeof(device_snapshot[0]));
    memcpy(service_snapshot, services, services_found * sizeof(service_snapshot[0]));
    memcpy(characteristic_snapshot, characteristics,
           characteristics_found * sizeof(characteristic_snapshot[0]));
    value_length = last_value_length;
    value_total = last_value_total;
    value_handle = last_value_handle;
    value_seen = last_value_seen;
    value_was_notification = last_value_was_notification;
    memcpy(value_snapshot, last_value, value_length);
    peer_index = selected_device;
    portEXIT_CRITICAL(&tool_lock);
    if (!ready) {
        set_status(screen_sd_available ? "Wait for discovery to finish before saving" :
                   "Insert a writable SD card and reboot before saving");
        return;
    }

    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    char day[7];
    strftime(day, sizeof(day), "%y%m%d", &local);
    char directory[32];
    snprintf(directory, sizeof(directory), "/sdcard/BLE/%s", day);
    if ((mkdir("/sdcard/BLE", 0775) != 0 && errno != EEXIST) ||
        (mkdir(directory, 0775) != 0 && errno != EEXIST)) {
        int error = errno ? errno : EIO;
        if (report_storage_error) report_storage_error(error);
        set_status("Evidence directory failed: %s", strerror(error));
        return;
    }

    char temporary_path[48] = "";
    char final_path[48] = "";
    int descriptor = -1;
    for (int collision = 0; collision < 10; collision++) {
        snprintf(temporary_path, sizeof(temporary_path), "%s/B%02d%02d%02d%d.TMP",
                 directory, local.tm_hour, local.tm_min, local.tm_sec, collision);
        snprintf(final_path, sizeof(final_path), "%s/B%02d%02d%02d%d.CSV",
                 directory, local.tm_hour, local.tm_min, local.tm_sec, collision);
        if (access(final_path, F_OK) == 0) continue;
        descriptor = open(temporary_path, O_WRONLY | O_CREAT | O_EXCL, 0664);
        if (descriptor >= 0 || errno != EEXIST) break;
    }
    if (descriptor < 0) {
        int error = errno ? errno : EEXIST;
        if (report_storage_error) report_storage_error(error);
        set_status("Evidence file could not be created: %s", strerror(error));
        return;
    }
    FILE *file = fdopen(descriptor, "wb");
    if (!file) {
        int error = errno ? errno : EIO;
        close(descriptor);
        unlink(temporary_path);
        if (report_storage_error) report_storage_error(error);
        set_status("Evidence file could not be opened: %s", strerror(error));
        return;
    }

    char peer_address[18] = "";
    char peer_name[33] = "";
    if (peer_index >= 0 && (size_t)peer_index < devices_found) {
        address_text(&device_snapshot[peer_index].address, peer_address);
        csv_text(device_snapshot[peer_index].name, peer_name, sizeof(peer_name));
    }
    bool ok = fprintf(file, "unix_time,event,address,name,rssi,service_uuid,"
                      "characteristic_uuid,handle,properties,value_hex\n") >= 0;
    for (size_t i = 0; ok && i < devices_found; i++) {
        char address[18];
        char name[33];
        address_text(&device_snapshot[i].address, address);
        csv_text(device_snapshot[i].name, name, sizeof(name));
        ok = fprintf(file, "%lld,advertisement,%s,%s,%d,,,,,\n",
                     (long long)now, address, name, device_snapshot[i].rssi) >= 0;
    }
    for (size_t i = 0; ok && i < characteristics_found; i++) {
        if (characteristic_snapshot[i].service_index >= services_found) continue;
        char service_uuid[BLE_UUID_STR_LEN];
        char characteristic_uuid[BLE_UUID_STR_LEN];
        char properties[16];
        ble_uuid_to_str(&service_snapshot[characteristic_snapshot[i].service_index].uuid.u,
                        service_uuid);
        ble_uuid_to_str(&characteristic_snapshot[i].uuid.u, characteristic_uuid);
        properties_text(characteristic_snapshot[i].properties, properties);
        ok = fprintf(file, "%lld,characteristic,%s,%s,,%s,%s,0x%04X,%s,\n",
                     (long long)now, peer_address, peer_name, service_uuid,
                     characteristic_uuid, characteristic_snapshot[i].val_handle,
                     properties) >= 0;
    }
    if (ok && value_seen) {
        char value_hex[BLE_TOOL_VALUE_MAX * 3 + 1] = "";
        size_t used = 0;
        for (size_t i = 0; i < value_length && used < sizeof(value_hex); i++)
            used += (size_t)snprintf(value_hex + used, sizeof(value_hex) - used,
                                     "%s%02X", i ? " " : "", value_snapshot[i]);
        ok = fprintf(file, "%lld,%s,%s,%s,,,,0x%04X,,%s%s\n", (long long)now,
                     value_was_notification ? "notification" : "read", peer_address, peer_name,
                     value_handle, value_hex, value_total > value_length ? " ..." : "") >= 0;
    }
    if (!ok && !errno) errno = EIO;
    if (storage_commit_new_file(&file, temporary_path, final_path) != 0) {
        int error = errno ? errno : EIO;
        if (report_storage_error) report_storage_error(error);
        set_status("Evidence save failed: %s", strerror(error));
        return;
    }
    set_status("Evidence saved to %s", final_path);
}

static void render_scan_view(void)
{
    ble_tool_device_t snapshot[BLE_TOOL_DEVICE_MAX];
    size_t count;
    int selected;
    bool scanning;
    bool available;
    bool pending;
    portENTER_CRITICAL(&tool_lock);
    count = device_count;
    memcpy(snapshot, devices, count * sizeof(snapshot[0]));
    selected = selected_device;
    scanning = scan_requested;
    available = tool_available;
    pending = connecting || stopping;
    portEXIT_CRITICAL(&tool_lock);

    lv_obj_t *controls = row(screen_parent, 84);
    lv_obj_t *scan = action_button(controls, scanning ? "STOP SCAN" : "START SCAN", 250,
                                   scan_clicked, NULL);
    if (!available || pending) lv_obj_add_state(scan, LV_STATE_DISABLED);
    if (selected >= 0 && (size_t)selected < count) {
        lv_obj_t *connect = action_button(controls, "CONNECT", 250, connect_clicked, NULL);
        if (pending) lv_obj_add_state(connect, LV_STATE_DISABLED);
    }
    for (size_t i = 0; i < count; i++) {
        char address[18];
        char text[96];
        address_text(&snapshot[i].address, address);
        snprintf(text, sizeof(text), "%s%s\n%s  %d dBm",
                 (int)i == selected ? LV_SYMBOL_OK "  " : "", snapshot[i].name,
                 address, snapshot[i].rssi);
        lv_obj_t *device = action_button(screen_parent, text, 620, device_clicked,
                                         (void *)(intptr_t)i);
        lv_obj_set_height(device, 86);
    }
    if (!count) {
        lv_obj_t *empty = lv_label_create(screen_parent);
        lv_obj_set_width(empty, 620);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(empty, scanning ? "Listening for advertisements..." :
                          "No scan results. Scanning never starts automatically.");
    }
}

static void render_characteristic_detail(const ble_tool_characteristic_t *characteristic,
                                         bool busy, uint16_t subscribed)
{
    char uuid[BLE_UUID_STR_LEN];
    char service_uuid[BLE_UUID_STR_LEN];
    char properties[16];
    ble_uuid_to_str(&characteristic->uuid.u, uuid);
    portENTER_CRITICAL(&tool_lock);
    ble_uuid_any_t service = services[characteristic->service_index].uuid;
    portEXIT_CRITICAL(&tool_lock);
    ble_uuid_to_str(&service.u, service_uuid);
    properties_text(characteristic->properties, properties);

    lv_obj_t *back_row = row(screen_parent, 80);
    action_button(back_row, LV_SYMBOL_LEFT "  CHARACTERISTICS", 300,
                  characteristics_back_clicked, NULL);
    lv_obj_t *details = lv_label_create(screen_parent);
    lv_obj_set_width(details, 620);
    lv_label_set_long_mode(details, LV_LABEL_LONG_WRAP);
    lv_label_set_text_fmt(details, "Service %s\nCharacteristic %s\nValue handle 0x%04X  Properties %s",
                          service_uuid, uuid, characteristic->val_handle, properties);

    lv_obj_t *actions = row(screen_parent, 84);
    lv_obj_t *read = action_button(actions, "READ", 180, read_clicked, NULL);
    lv_obj_t *subscribe = action_button(actions,
        subscribed == characteristic->val_handle ? "UNSUBSCRIBE" : "SUBSCRIBE",
        220, subscribe_clicked, NULL);
    if (!(characteristic->properties & BLE_GATT_CHR_PROP_READ) || busy)
        lv_obj_add_state(read, LV_STATE_DISABLED);
    if (!(characteristic->properties & (BLE_GATT_CHR_PROP_NOTIFY | BLE_GATT_CHR_PROP_INDICATE)) || busy)
        lv_obj_add_state(subscribe, LV_STATE_DISABLED);

    value_label = lv_label_create(screen_parent);
    lv_obj_set_width(value_label, 620);
    lv_label_set_long_mode(value_label, LV_LABEL_LONG_WRAP);
    char value[512];
    format_value(value, sizeof(value));
    lv_label_set_text(value_label, value);

    lv_obj_t *warning = lv_label_create(screen_parent);
    lv_obj_set_width(warning, 620);
    lv_obj_set_style_text_align(warning, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(warning, "Raw writes can change or damage a device.\n"
                               "The exact same value must be tapped twice within five seconds.");
    write_area = lv_textarea_create(screen_parent);
    lv_obj_set_size(write_area, 620, 72);
    lv_textarea_set_one_line(write_area, true);
    lv_textarea_set_max_length(write_area, BLE_TOOL_WRITE_MAX * 3 - 1);
    lv_textarea_set_accepted_chars(write_area, "0123456789abcdefABCDEF ");
    lv_textarea_set_placeholder_text(write_area, "Hex bytes, for example: 01 AF 00");
    lv_obj_t *write = action_button(screen_parent, "WRITE RAW BYTES", 620, write_clicked, NULL);
    if (!(characteristic->properties & (BLE_GATT_CHR_PROP_WRITE | BLE_GATT_CHR_PROP_WRITE_NO_RSP)) || busy)
        lv_obj_add_state(write, LV_STATE_DISABLED);
    lv_obj_t *keyboard = lv_keyboard_create(screen_parent);
    lv_obj_set_size(keyboard, 620, 330);
    lv_keyboard_set_textarea(keyboard, write_area);
}

static void render_connected_view(void)
{
    ble_tool_characteristic_t characteristic_snapshot[BLE_TOOL_CHARACTERISTIC_MAX];
    ble_tool_service_t service_snapshot[BLE_TOOL_SERVICE_MAX];
    size_t count;
    size_t services_found;
    int selected;
    bool in_discovery;
    bool busy;
    bool disconnecting;
    bool sd_available;
    uint16_t subscribed;
    portENTER_CRITICAL(&tool_lock);
    count = characteristic_count;
    services_found = service_count;
    memcpy(characteristic_snapshot, characteristics, count * sizeof(characteristic_snapshot[0]));
    memcpy(service_snapshot, services, services_found * sizeof(service_snapshot[0]));
    selected = selected_characteristic;
    in_discovery = discovering;
    busy = operation_busy;
    disconnecting = stopping;
    sd_available = screen_sd_available;
    subscribed = subscribed_value_handle;
    portEXIT_CRITICAL(&tool_lock);

    lv_obj_t *controls = row(screen_parent, 82);
    lv_obj_t *disconnect = action_button(controls, disconnecting ? "DISCONNECTING..." : "DISCONNECT",
                                         260, disconnect_clicked, NULL);
    lv_obj_t *save = action_button(controls, "SAVE EVIDENCE", 260,
                                   save_evidence_clicked, NULL);
    if (disconnecting) lv_obj_add_state(disconnect, LV_STATE_DISABLED);
    if (!sd_available || in_discovery || disconnecting || busy)
        lv_obj_add_state(save, LV_STATE_DISABLED);
    if (selected >= 0 && (size_t)selected < count) {
        render_characteristic_detail(&characteristic_snapshot[selected], busy, subscribed);
        return;
    }
    lv_obj_t *summary = lv_label_create(screen_parent);
    lv_obj_set_width(summary, 620);
    lv_obj_set_style_text_align(summary, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text_fmt(summary, in_discovery ? "Discovering GATT database..." :
                          "%u services, %u characteristics. Tap one to inspect.",
                          (unsigned)services_found, (unsigned)count);
    for (size_t i = 0; i < count; i++) {
        char service_uuid[BLE_UUID_STR_LEN];
        char uuid[BLE_UUID_STR_LEN];
        char properties[16];
        char text[160];
        ble_uuid_to_str(&service_snapshot[characteristic_snapshot[i].service_index].uuid.u,
                        service_uuid);
        ble_uuid_to_str(&characteristic_snapshot[i].uuid.u, uuid);
        properties_text(characteristic_snapshot[i].properties, properties);
        snprintf(text, sizeof(text), "S %s\n0x%04X  %s  [%s]",
                 service_uuid, characteristic_snapshot[i].val_handle, uuid, properties);
        lv_obj_t *button = action_button(screen_parent, text, 620, characteristic_clicked,
                                         (void *)(intptr_t)i);
        lv_obj_set_height(button, 94);
    }
}

static void render(void)
{
    if (!screen_parent || !screen_active) return;
    lv_obj_clean(screen_parent);
    status_label = value_label = write_area = NULL;
    lv_obj_t *title = lv_label_create(screen_parent);
    lv_label_set_text(title, "BLE GATT Explorer");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_t *help = lv_label_create(screen_parent);
    lv_obj_set_width(help, 620);
    lv_obj_set_style_text_align(help, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(help, "Scanning and connections are always explicit.\n"
                            "Only connect to devices you own; pairing is not automated.\n"
                            "Discovery is capped at 8 devices, 16 services, and 32 characteristics.\n"
                            "A saved snapshot includes nearby device names and addresses.");
    status_label = lv_label_create(screen_parent);
    lv_obj_set_width(status_label, 620);
    lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
    char status[sizeof(status_text)];
    bool connected_snapshot;
    portENTER_CRITICAL(&tool_lock);
    snprintf(status, sizeof(status), "%s", status_text);
    connected_snapshot = connected || stopping;
    portEXIT_CRITICAL(&tool_lock);
    lv_label_set_text(status_label, status);
    if (connected_snapshot) render_connected_view();
    else render_scan_view();
}

static void tick(lv_timer_t *timer)
{
    (void)timer;
    bool rebuild;
    bool update_status;
    bool update_value;
    bool active;
    bool expired = false;
    char status[sizeof(status_text)];
    portENTER_CRITICAL(&tool_lock);
    active = screen_active;
    rebuild = view_dirty;
    view_dirty = false;
    update_status = status_dirty;
    if (update_status) {
        snprintf(status, sizeof(status), "%s", status_text);
        status_dirty = false;
    }
    update_value = value_dirty;
    value_dirty = false;
    if (write_armed && (uint32_t)(lv_tick_get() - write_armed_at) > BLE_TOOL_CONFIRM_MS) {
        write_armed = false;
        expired = true;
    }
    portEXIT_CRITICAL(&tool_lock);
    if (!active) return;
    if (expired) set_status("Write confirmation expired; nothing was sent");
    if (rebuild) {
        render();
        return;
    }
    if (update_status && status_label) lv_label_set_text(status_label, status);
    if (update_value && value_label) {
        char value[512];
        format_value(value, sizeof(value));
        lv_label_set_text(value_label, value);
    }
}

void ble_tool_show(lv_obj_t *parent, bool available, bool sd_available,
                   ble_tool_scan_refresh_cb_t scan_refresh,
                   ble_tool_connect_allowed_cb_t connect_allowed,
                   ble_tool_storage_error_cb_t storage_error_cb)
{
    screen_parent = parent;
    refresh_scan = scan_refresh;
    connection_allowed = connect_allowed;
    report_storage_error = storage_error_cb;
    portENTER_CRITICAL(&tool_lock);
    screen_active = true;
    tool_available = available;
    screen_sd_available = sd_available;
    scan_requested = false;
    device_count = 0;
    selected_device = -1;
    write_armed = false;
    view_dirty = true;
    status_dirty = value_dirty = false;
    bool pending = connecting || connected || stopping;
    portEXIT_CRITICAL(&tool_lock);
    set_status(pending ? "Previous BLE connection is still stopping..." :
               available ? "Ready; scanning and connections are opt-in" :
               "Bluetooth hardware is unavailable");
    render();
    ui_timer = lv_timer_create(tick, 200, NULL);
}

void ble_tool_stop(void)
{
    if (ui_timer) {
        lv_timer_delete(ui_timer);
        ui_timer = NULL;
    }
    uint16_t handle;
    bool cancel;
    bool terminate;
    portENTER_CRITICAL(&tool_lock);
    screen_active = false;
    scan_requested = false;
    write_armed = false;
    cancel = connecting;
    terminate = connected;
    handle = connection_handle;
    if (cancel || terminate) stopping = true;
    else stopping = false;
    portEXIT_CRITICAL(&tool_lock);
    screen_parent = status_label = value_label = write_area = NULL;
    if (cancel) {
        int rc = ble_gap_conn_cancel();
        if (rc) ESP_LOGW(TAG, "Connection cancel failed: %d", rc);
    } else if (terminate) {
        int rc = ble_gap_terminate(handle, BLE_ERR_REM_USER_CONN_TERM);
        if (rc) ESP_LOGW(TAG, "Disconnect request failed: %d", rc);
    }
    if (refresh_scan) refresh_scan();
}

bool ble_tool_busy(void)
{
    portENTER_CRITICAL(&tool_lock);
    bool busy = connecting || connected || stopping || operation_busy;
    portEXIT_CRITICAL(&tool_lock);
    return busy;
}

bool ble_tool_scan_requested(void)
{
    portENTER_CRITICAL(&tool_lock);
    bool requested = tool_available && screen_active && scan_requested &&
                     !connecting && !connected && !stopping;
    portEXIT_CRITICAL(&tool_lock);
    return requested;
}

void ble_tool_observe_advertisement(const ble_addr_t *address, int8_t rssi,
                                    const uint8_t *name, uint8_t name_length)
{
    if (!address) return;
    char sanitized[33];
    sanitize_name(name, name_length, sanitized);
    portENTER_CRITICAL(&tool_lock);
    if (!screen_active || !scan_requested || connecting || connected || stopping) {
        portEXIT_CRITICAL(&tool_lock);
        return;
    }
    for (size_t i = 0; i < device_count; i++) {
        if (address_equal(&devices[i].address, address)) {
            devices[i].rssi = rssi;
            if (strcmp(sanitized, "(unnamed)")) snprintf(devices[i].name, sizeof(devices[i].name), "%s", sanitized);
            portEXIT_CRITICAL(&tool_lock);
            return;
        }
    }
    if (device_count < BLE_TOOL_DEVICE_MAX) {
        devices[device_count].address = *address;
        devices[device_count].rssi = rssi;
        snprintf(devices[device_count].name, sizeof(devices[device_count].name), "%s", sanitized);
        device_count++;
        view_dirty = true;
    }
    portEXIT_CRITICAL(&tool_lock);
}

void ble_tool_self_test(void)
{
    uint8_t value[BLE_TOOL_WRITE_MAX];
    size_t length;
    assert(parse_hex("01 af FF", value, &length) && length == 3 &&
           value[0] == 1 && value[1] == 0xaf && value[2] == 0xff);
    assert(!parse_hex("0", value, &length));
    assert(!parse_hex("GG", value, &length));
    char name[33];
    const uint8_t raw_name[] = {'T', 'a', 'b', 1, '5', ' '};
    sanitize_name(raw_name, sizeof(raw_name), name);
    assert(strcmp(name, "Tab?5") == 0);
    uint8_t armed[] = {1, 2};
    assert(confirmation_valid(true, 7, 7, armed, 2, armed, 2, 100, 5100));
    assert(!confirmation_valid(true, 7, 8, armed, 2, armed, 2, 100, 5100));
    assert(!confirmation_valid(true, 7, 7, armed, 2, armed, 2, 100, 5101));
    char properties[16];
    properties_text(BLE_GATT_CHR_PROP_READ | BLE_GATT_CHR_PROP_NOTIFY, properties);
    assert(strcmp(properties, "RN") == 0);
    char csv[16];
    csv_text("a,b\"c\n", csv, sizeof(csv));
    assert(strcmp(csv, "a_b_c_") == 0);
}
