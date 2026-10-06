#pragma once
#include "compat.h"
extern volatile LONG mqtt_net_allocations, mqtt_net_transports, mqtt_net_threads;
extern unsigned mqtt_net_read_cap;
extern DWORD mqtt_net_ui_thread;
extern volatile LONG mqtt_net_data_events, mqtt_net_published_events, mqtt_net_subscribed_events;
extern volatile LONG mqtt_net_held_api_returns;
typedef struct { int offset, length, total, topic_length, qos, retain, dup; } mqtt_net_data_record_t;
extern mqtt_net_data_record_t mqtt_net_data_records[128];
void mqtt_net_join_task(void);
void mqtt_net_shutdown_worker(void);
bool mqtt_net_on_worker(void);
void mqtt_net_hold_api_return(int event_id);
