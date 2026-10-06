#pragma once
#include "host.h"
#include "mqtt_client.h"
#include <stdio.h>
typedef enum {
    MQTT_FILE_OK, MQTT_FILE_REPAIR, MQTT_FILE_OPEN, MQTT_FILE_FLUSH,
    MQTT_FILE_SYNC, MQTT_FILE_CLOSE,
} mqtt_file_fault_t;
typedef enum {
    MQTT_NVS_OK, MQTT_NVS_OPEN_READ, MQTT_NVS_OPEN_WRITE, MQTT_NVS_SIZE,
    MQTT_NVS_READ, MQTT_NVS_READ_SHORT, MQTT_NVS_READ_GONE,
    MQTT_NVS_SET_BEFORE, MQTT_NVS_SET_AFTER, MQTT_NVS_COMMIT,
    MQTT_NVS_ERASE_BEFORE, MQTT_NVS_ERASE_AFTER,
} mqtt_nvs_fault_t;
extern volatile LONG mqtt_host_nvs_handles, mqtt_host_nvs_reads, mqtt_host_nvs_commits;
extern volatile LONG mqtt_host_nvs_erases;
void mqtt_host_nvs_fault(mqtt_nvs_fault_t fault);
void mqtt_host_nvs_seed(const void *data, size_t length, bool wrong_type);
bool mqtt_host_nvs_present(void);
extern volatile LONG mqtt_host_clients, mqtt_host_allocations, mqtt_host_stop_entered;
extern volatile LONG mqtt_host_destroy_entered, mqtt_host_sync_entered, mqtt_host_close_entered;
extern volatile LONG mqtt_host_open_files, mqtt_host_appends, mqtt_host_subscribes, mqtt_host_publishes;
extern volatile LONG mqtt_host_initializations, mqtt_host_disconnects, mqtt_host_nvs_writes;
extern volatile LONG mqtt_host_fail_allocation;
extern volatile LONG mqtt_host_actions_entered, mqtt_host_actions_active, mqtt_host_dispatch_entered;
extern volatile LONG mqtt_host_log_dispatch_entered;
extern DWORD mqtt_host_ui_thread;
typedef struct {
    bool certificate_bundle;
    char uri[256], username[261], password[261];
} mqtt_host_connection_record_t;
typedef struct {
    uint8_t version;
    char uri[256], username[65], password[65], topic[128];
} mqtt_host_profile_record_t;
extern mqtt_host_connection_record_t mqtt_host_connection;
extern mqtt_host_profile_record_t mqtt_host_profile;
typedef struct {
    bool publish, store;
    int qos, retained, length, result;
    char topic[128], payload[513];
} mqtt_host_action_record_t;
extern mqtt_host_action_record_t mqtt_host_actions[256];
extern HANDLE mqtt_host_stop_release, mqtt_host_destroy_release, mqtt_host_file_release;
extern HANDLE mqtt_host_action_release, mqtt_host_dispatch_release;
extern HANDLE mqtt_host_log_dispatch_release;
extern bool mqtt_host_hold_stop, mqtt_host_hold_destroy, mqtt_host_hold_sync, mqtt_host_hold_close;
extern bool mqtt_host_hold_action, mqtt_host_hold_dispatch;
extern bool mqtt_host_hold_log_dispatch;
void mqtt_host_setup(const char *output, const char *mode);
void mqtt_host_shutdown(void);
void mqtt_host_event(esp_mqtt_event_id_t event);
void mqtt_host_event_data(esp_mqtt_event_id_t event_id, esp_mqtt_event_t *event);
void mqtt_host_receive(const char *topic, const char *data);
void mqtt_host_data(esp_mqtt_event_t *event);
void mqtt_host_file_fault(mqtt_file_fault_t fault);
const char *mqtt_host_csv_path(void);
struct tm *mqtt_host_localtime_r(const time_t *, struct tm *);
int mqtt_host_mkdir(const char *);
FILE *mqtt_host_fopen(const char *, const char *);
int mqtt_host_fclose(FILE *);
int mqtt_host_fflush(FILE *);
int mqtt_host_commit(int);
