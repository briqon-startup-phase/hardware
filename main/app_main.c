#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <time.h>
#include <math.h>
#include <assert.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_crt_bundle.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "briqon_core.h"
#include "sensor_adapter.h"
#include "contract.h"

#define FW_VERSION "0.1.1"
#define RELAY_GPIO 5
#define LOCK() xSemaphoreTake(state_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(state_lock)
static const char *TAG = "briqon";
static SemaphoreHandle_t state_lock;
static QueueHandle_t command_queue, overflow_queue, rejection_queue, notification_queue;
static esp_mqtt_client_handle_t mqtt;
static bq_assembly_t assembly; /* exclusively owned by MQTT callback */
static char device_id[65], boot_id[40], base_topic[160], command_topic[176];
static char client_id[112], will_json[320];
static TaskHandle_t acquisition_handle, control_handle, publisher_handle, supervisor_handle;

typedef struct {
    uint64_t seq, uptime_ms;
    time_t utc;
    char session[40];
} stamp_t;
typedef struct {
    stamp_t stamp;
    bool notification, output_on;
    char command_id[65], action[24], status[12], reason[64];
    unsigned transition, episode;
    float temperature, threshold;
    bool valid;
    uint32_t config_version;
} outbound_t;
typedef struct {
    char payload[BQ_PAYLOAD_MAX + 1], session[40];
    size_t length;
    bool retained;
} inbound_t;
typedef struct {
    bool used, accepted_pending, terminal_pending;
    uint64_t order;
    bq_command_t command;
    outbound_t accepted, terminal;
} cache_t;
static cache_t cache[BQ_COMMAND_CACHE];
static uint64_t cache_order;
typedef struct {
    bq_config_t config;
    bq_rules_t rules;
    char session[40], last_command[65], last_status[12];
    bool ip_ready, wifi_connected, wifi_connecting;
    bool mqtt_connected, ready, output_on, requested_on, config_error;
    bool sample_valid, have_valid;
    float temperature, last_valid;
    uint64_t seq, sample_seq, sample_ms, last_valid_ms, loss_ms;
    time_t sample_utc;
    uint64_t acquisition_progress, control_progress, publish_progress;
    uint32_t sample_count, malformed, busy, dropped_notifications, dropped_results;
    uint32_t publish_failures, coalesced, reconnects, sensor_errors;
    int subscription_id;
} state_t;
static state_t state;
static uint64_t now_ms(void) { return (uint64_t)(esp_timer_get_time() / 1000); }
static state_t snapshot(void) { state_t s; LOCK(); s = state; UNLOCK(); return s; }
static void random_identity(char *dst, size_t len, const char *prefix) {
    snprintf(dst,len,"%s-%08" PRIx32 "%08" PRIx32,prefix,esp_random(),esp_random());
}
static stamp_t stamp(void) {
    stamp_t s = {.uptime_ms=now_ms(), .utc=time(NULL)};
    LOCK(); s.seq = ++state.seq; strlcpy(s.session,state.session,sizeof(s.session)); UNLOCK();
    return s;
}
static void add_time(cJSON *j, const char *key, time_t utc) {
    if (utc < 1704067200) { cJSON_AddNullToObject(j,key); return; }
    struct tm tm;
    char formatted[32];
    gmtime_r(&utc,&tm);
    strftime(formatted,sizeof(formatted),"%Y-%m-%dT%H:%M:%SZ",&tm);
    cJSON_AddStringToObject(j,key,formatted);
}
static cJSON *envelope(stamp_t s) {
    cJSON *j = cJSON_CreateObject();
    if (!j) return NULL;
    cJSON_AddNumberToObject(j,"schema_version",1);
    cJSON_AddStringToObject(j,"device_id",device_id);
    cJSON_AddStringToObject(j,"boot_id",boot_id);
    cJSON_AddStringToObject(j,"session_id",s.session);
    cJSON_AddNumberToObject(j,"seq",(double)s.seq);
    cJSON_AddNumberToObject(j,"uptime_ms",(double)s.uptime_ms);
    add_time(j,"observed_at",s.utc);
    cJSON_AddStringToObject(j,"time_quality",s.utc >= 1704067200 ? "synchronized" : "unsynchronized");
    return j;
}
static void add_output(cJSON *j, bool on) {
    cJSON *o = cJSON_AddObjectToObject(j,"output");
    cJSON_AddStringToObject(o,"channel","relay-1");
    cJSON_AddStringToObject(o,"logical_state",on ? "ON" : "OFF");
    cJSON_AddStringToObject(o,"verification","gpio_reported");
}
static void add_config(cJSON *j, bq_config_t c) {
    cJSON_AddNumberToObject(j,"config_version",c.version);
    cJSON *o = cJSON_AddObjectToObject(j,"config");
    cJSON_AddNumberToObject(o,"sample_interval_ms",c.sample_interval_ms);
    cJSON_AddNumberToObject(o,"telemetry_interval_ms",c.telemetry_interval_ms);
    cJSON_AddNumberToObject(o,"high_c",c.high_c);
    cJSON_AddNumberToObject(o,"clear_c",c.clear_c);
    cJSON_AddNumberToObject(o,"dwell_ms",c.dwell_ms);
}
/* Only the publisher calls enqueue. A full outbox never causes a blocking publish. */
static bool publish_json(const char *suffix, cJSON *j, int qos, bool retain) {
    if (!j) return false;
    char topic[192], payload[2049];
    snprintf(topic,sizeof(topic),"%s/%s",base_topic,suffix);
    bool ok = cJSON_PrintPreallocated(j,payload,sizeof(payload),false);
    cJSON_Delete(j);
    if (ok) {
        size_t size = strlen(payload);
        int queued=esp_mqtt_client_get_outbox_size(mqtt);
        /* ESP-MQTT enqueue requires store=true even for QoS0. Only allow one
           QoS0 message to enter an empty outbox; newer samples stay coalesced. */
        if ((qos == 0 && queued > 0) ||
            (size_t)queued + size + strlen(topic) + 64 > BQ_OUTBOX_MAX)
            ok = false;
        else ok = esp_mqtt_client_enqueue(mqtt,topic,payload,(int)size,qos,retain,true) >= 0;
    }
    if (!ok) { LOCK(); state.publish_failures++; UNLOCK(); }
    return ok;
}
static cJSON *event_json(const outbound_t *e) {
    cJSON *j = envelope(e->stamp);
    if (!j) return NULL;
    cJSON_AddNumberToObject(j,"config_version",e->config_version);
    if (!e->notification) {
        cJSON_AddStringToObject(j,"command_id",e->command_id);
        cJSON_AddStringToObject(j,"action",e->action);
        cJSON_AddStringToObject(j,"status",e->status);
        cJSON_AddStringToObject(j,"reason",e->reason);
        add_output(j,e->output_on);
        return j;
    }
    bool high = e->transition == BQ_HIGH_OPEN || e->transition == BQ_HIGH_CLEAR;
    bool opened = e->transition == BQ_HIGH_OPEN || e->transition == BQ_FAULT_OPEN;
    const char *code = high ? "temperature_high" : "sensor_fault";
    char episode_id[128], notification_id[144];
    snprintf(episode_id,sizeof(episode_id),"%s:%s:%u",boot_id,code,e->episode);
    snprintf(notification_id,sizeof(notification_id),"%s:%s",episode_id,opened ? "opened" : "cleared");
    cJSON_AddStringToObject(j,"notification_id",notification_id);
    cJSON_AddStringToObject(j,"episode_id",episode_id);
    cJSON_AddStringToObject(j,"code",code);
    cJSON_AddStringToObject(j,"transition",opened ? "opened" : "cleared");
    cJSON_AddStringToObject(j,"severity",opened ? "warning" : "info");
    cJSON_AddStringToObject(j,"sensor_id","temperature-1");
    if (e->valid) cJSON_AddNumberToObject(j,"value",e->temperature);
    else cJSON_AddNullToObject(j,"value");
    if (high) cJSON_AddNumberToObject(j,"threshold",e->threshold);
    return j;
}
static outbound_t result(const bq_command_t *c, const char *status, const char *reason) {
    state_t s = snapshot();
    outbound_t e = {.stamp=stamp(), .output_on=s.output_on, .config_version=s.config.version};
    strlcpy(e.command_id,c->command_id[0] ? c->command_id : "unavailable",sizeof(e.command_id));
    strlcpy(e.action,bq_action_name(c->action),sizeof(e.action));
    strlcpy(e.status,status,sizeof(e.status)); strlcpy(e.reason,reason,sizeof(e.reason));
    return e;
}
static void reject(const bq_command_t *c, const char *reason) {
    outbound_t e = result(c,"rejected",reason);
    if (xQueueSend(rejection_queue,&e,0) != pdTRUE) {
        LOCK(); state.dropped_results++; UNLOCK();
    }
    ESP_LOGW(TAG,"command rejected id=%s reason=%s",e.command_id,reason);
}
static void lost_readiness(void) { /* call with state_lock held */
    if (state.ready || !state.loss_ms) state.loss_ms = now_ms();
    state.ready = false;
}
static void mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base;
    esp_mqtt_event_handle_t e = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED: {
        LOCK(); state.mqtt_connected = true; state.reconnects++; UNLOCK();
        int sub = esp_mqtt_client_subscribe(mqtt,command_topic,1);
        LOCK(); state.subscription_id = sub; UNLOCK();
        ESP_LOGI(TAG,"MQTT connected; waiting SUBACK id=%d",sub);
        break;
    }
    case MQTT_EVENT_SUBSCRIBED:
        LOCK();
        if (e->msg_id == state.subscription_id && e->msg_id >= 0 &&
            e->data && e->data_len == 1 && (uint8_t)e->data[0] <= 1) {
            state.ready = true; state.loss_ms = 0;
        }
        UNLOCK();
        ESP_LOGI(TAG,"SUBACK id=%d",e->msg_id);
        break;
    case MQTT_EVENT_DISCONNECTED:
        assembly.active = false;
        LOCK();
        state.mqtt_connected = false; lost_readiness();
        /* A prior enqueue is not delivery proof. Reconcile bounded cached terminal
           results after recovery, preserving their original message identity. */
        for (int i=0; i<BQ_COMMAND_CACHE; ++i)
            if (cache[i].used && cache[i].terminal.status[0]) cache[i].terminal_pending=true;
        UNLOCK();
        ESP_LOGW(TAG,"MQTT disconnected");
        break;
    case MQTT_EVENT_DATA: {
        bool match = e->topic_len == (int)strlen(command_topic) && e->topic &&
            !memcmp(e->topic,command_topic,(size_t)e->topic_len);
        int complete = bq_assemble(&assembly,e->data,e->data_len,e->current_data_offset,
            e->total_data_len,match,e->retain,now_ms());
        if (complete < 0) { LOCK(); state.malformed++; UNLOCK(); }
        if (complete == 1) {
            /* Static scratch avoids a 2 KiB callback-stack copy. Callback is serial. */
            static inbound_t frame;
            memcpy(frame.payload,assembly.payload,assembly.used + 1);
            frame.length = assembly.used; frame.retained = assembly.retained;
            LOCK(); strlcpy(frame.session,state.session,sizeof(frame.session)); UNLOCK();
            if (xQueueSend(command_queue,&frame,0) != pdTRUE) {
                LOCK(); state.busy++; UNLOCK();
                if (xQueueSend(overflow_queue,&frame,0) != pdTRUE) {
                    LOCK(); state.dropped_results++; UNLOCK();
                }
            }
        }
        break;
    }
    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG,"MQTT transport error (credentials omitted)");
        break;
    default: break;
    }
}
static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)data;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        LOCK(); state.ip_ready = true; state.wifi_connected = true; state.wifi_connecting = false; UNLOCK();
        ESP_LOGI(TAG,"Wi-Fi IP ready");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        LOCK(); state.wifi_connected = true; state.wifi_connecting = false; UNLOCK();
        ESP_LOGI(TAG,"Wi-Fi associated; waiting for IP");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        LOCK();
        state.ip_ready = false; state.wifi_connected = false; state.wifi_connecting = false;
        state.mqtt_connected = false; lost_readiness();
        UNLOCK();
        ESP_LOGW(TAG,"Wi-Fi disconnected");
    }
}
static void write_output(bool on, const char *reason) { /* control task only */
#ifdef CONFIG_BRIQON_RELAY_ACTIVE_LOW
    int level = on ? 0 : 1;
#else
    int level = on ? 1 : 0;
#endif
    ESP_ERROR_CHECK(gpio_set_level(RELAY_GPIO,level));
    LOCK(); state.output_on = on; UNLOCK();
    ESP_LOGI(TAG,"relay=%s gpio=%d verification=gpio_reported reason=%s",on ? "ON":"OFF",level,reason);
}
static bool persist_config(const bq_config_t *c) {
    /* Single atomic NVS blob plus NVS's own CRC. No write per sensor sample. */
    nvs_handle_t nvs;
    if (nvs_open("briqon",NVS_READWRITE,&nvs) != ESP_OK) return false;
    esp_err_t err = nvs_set_blob(nvs,"config_v1",c,sizeof(*c));
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err == ESP_OK;
}
static void load_config(void) {
    state.config = bq_default_config();
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) { state.config_error = true; return; }
    nvs_handle_t nvs;
    err = nvs_open("briqon",NVS_READONLY,&nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) return;
    if (err != ESP_OK) { state.config_error = true; return; }
    bq_config_t config;
    size_t size = sizeof(config);
    err = nvs_get_blob(nvs,"config_v1",&config,&size);
    nvs_close(nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) return;
    if (err != ESP_OK || size != sizeof(config) || !bq_config_valid(&config))
        state.config_error = true;
    else state.config = config;
}
static void task_started(const char *name) {
    ESP_LOGI(TAG,"task=%s core=%d",name,xPortGetCoreID());
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
}
static void notify_rules(unsigned changes, state_t s) {
    for (unsigned bit=1; bit<=BQ_FAULT_CLEAR; bit <<= 1) {
        if (!(changes & bit)) continue;
        outbound_t e = {.stamp=stamp(), .notification=true, .transition=bit,
            .temperature=s.temperature, .valid=s.sample_valid, .config_version=s.config.version};
        bool high = bit == BQ_HIGH_OPEN || bit == BQ_HIGH_CLEAR;
        e.episode = high ? s.rules.high_episode : s.rules.fault_episode;
        e.threshold = bit == BQ_HIGH_OPEN ? s.config.high_c : s.config.clear_c;
        if (xQueueSend(notification_queue,&e,0) != pdTRUE) {
            LOCK(); state.dropped_notifications++; UNLOCK();
        }
        ESP_LOGI(TAG,"rule transition=%u episode=%u",bit,e.episode);
    }
}
static void acquisition_task(void *unused) {
    (void)unused;
    task_started("acquisition");
    esp_err_t init = bq_sensor_init();
    ESP_LOGI(TAG,"sensor init=%s",esp_err_to_name(init));
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        state_t before = snapshot();
        float temperature = NAN;
        esp_err_t err = bq_sensor_read(&temperature);
        uint64_t sample_ms = now_ms();
        LOCK();
        state.sample_valid = err == ESP_OK;
        state.temperature = temperature;
        state.sample_ms = sample_ms;
        state.sample_utc = time(NULL);
        state.sample_seq++;
        state.sample_count++;
        state.acquisition_progress = sample_ms;
        if (state.sample_valid) {
            state.have_valid = true; state.last_valid = temperature; state.last_valid_ms = sample_ms;
        } else state.sensor_errors++;
        unsigned changes = bq_rules_sample(&state.rules,&state.config,state.sample_valid,temperature,sample_ms);
        state_t after = state;
        UNLOCK();
        notify_rules(changes,after);
        if (err == ESP_OK) ESP_LOGI(TAG,"sample=%" PRIu64 " temperature=%.3f C",after.sample_seq,(double)temperature);
        else ESP_LOGW(TAG,"sample=%" PRIu64 " invalid reason=%s",after.sample_seq,esp_err_to_name(err));
        esp_task_wdt_reset();
        /* Short waits keep this task's own watchdog honest at 60 s intervals. */
        TickType_t target = wake + pdMS_TO_TICKS(before.config.sample_interval_ms);
        while ((int32_t)(target - xTaskGetTickCount()) > 0) {
            TickType_t left = target - xTaskGetTickCount();
            vTaskDelay(left < pdMS_TO_TICKS(250) ? left : pdMS_TO_TICKS(250));
            esp_task_wdt_reset();
        }
        wake = (int32_t)(xTaskGetTickCount() - target) > (int32_t)pdMS_TO_TICKS(1000) ? xTaskGetTickCount() : target;
    }
}
static const char *validate_target(const bq_command_t *c, const inbound_t *frame, state_t s) {
    if (frame->retained) return "retained_command";
    if (strcmp(c->device_id,device_id)) return "wrong_device";
    if (strcmp(c->boot_id,boot_id)) return "wrong_boot";
    if (strcmp(c->session_id,s.session) || strcmp(frame->session,s.session)) return "wrong_session";
    if (!s.ready) return "not_ready";
    if (!bq_deadline_valid(c->expires_ms,now_ms())) return "expired_or_future_deadline";
    return NULL;
}
static void handle_command(const inbound_t *frame, bool busy) {
    bq_command_t command;
    const char *error = bq_parse_command(frame->payload,frame->length,&command);
    if (error) {
        LOCK(); state.malformed++; UNLOCK();
        reject(&command,error); return;
    }
    state_t s = snapshot();
    if ((error = validate_target(&command,frame,s))) { reject(&command,error); return; }
    int slot = -1, duplicate = -1;
    uint64_t oldest = UINT64_MAX;
    LOCK();
    for (int i=0; i<BQ_COMMAND_CACHE; ++i) {
        if (cache[i].used && !strcmp(cache[i].command.command_id,command.command_id) &&
            !strcmp(cache[i].command.session_id,command.session_id)) { duplicate = i; break; }
        if (!cache[i].used) { if (oldest != 0) { slot=i; oldest=0; } }
        else if (!cache[i].accepted_pending && !cache[i].terminal_pending && cache[i].order < oldest) {
            oldest = cache[i].order; slot = i;
        }
    }
    if (duplicate >= 0) {
        bool equal = !memcmp(&cache[duplicate].command,&command,sizeof(command));
        if (equal) cache[duplicate].terminal_pending = true;
        UNLOCK();
        if (!equal) reject(&command,"command_id_conflict");
        return;
    }
    UNLOCK();
    if (busy || slot < 0) { reject(&command,"busy"); return; }
    if (s.config_error) { reject(&command,"config_error"); return; }
    if (command.action == BQ_SET_OUTPUT) {
#ifndef CONFIG_BRIQON_CONTROL_ENABLED
        reject(&command,"control_disabled"); return;
#endif
    }
    bq_config_t next = s.config;
    if (command.action == BQ_SET_CONFIG && !bq_config_patch(&s.config,&command,&next)) {
        reject(&command,command.expected_version != s.config.version ? "stale_config_version" : "invalid_config");
        return;
    }
    outbound_t accepted = result(&command,"accepted","validated");
    LOCK();
    cache[slot] = (cache_t){.used=true, .accepted_pending=true, .order=++cache_order,
        .command=command, .accepted=accepted};
    strlcpy(state.last_command,command.command_id,sizeof(state.last_command));
    strlcpy(state.last_status,"accepted",sizeof(state.last_status));
    if (command.action == BQ_SET_OUTPUT) state.requested_on = command.output_on;
    UNLOCK();
    /* The only GPIO/NVS owner rechecks time/session immediately before execution. */
    s = snapshot();
    error = validate_target(&command,frame,s);
    const char *terminal_status = "applied", *terminal_reason = "completed";
    if (error) { terminal_status="rejected"; terminal_reason=error; }
    else if (command.action == BQ_SET_OUTPUT) write_output(command.output_on,"command");
    else if (command.action == BQ_SET_CONFIG) {
        uint64_t started = now_ms();
        if (!persist_config(&next)) { terminal_status="failed"; terminal_reason="nvs_write_failed"; }
        else {
            LOCK(); state.config = next; UNLOCK();
            ESP_LOGI(TAG,"config persisted version=%" PRIu32 " elapsed_ms=%" PRIu64,next.version,now_ms()-started);
        }
    }
    LOCK(); strlcpy(state.last_status,terminal_status,sizeof(state.last_status)); UNLOCK();
    outbound_t terminal = result(&command,terminal_status,terminal_reason);
    LOCK(); cache[slot].terminal = terminal; cache[slot].terminal_pending = true; UNLOCK();
    ESP_LOGI(TAG,"command id=%s status=%s",command.command_id,terminal_status);
}
static void control_task(void *unused) {
    (void)unused;
    task_started("control");
    write_output(false,"boot");
    for (;;) {
        /* Independent of the network supervisor: even a slow reconnect cannot delay OFF. */
        state_t s = snapshot();
        if (!s.ready && s.output_on && s.loss_ms && now_ms()-s.loss_ms >= 5000)
            write_output(false,"readiness_lost_5s");
        inbound_t frame;
        if (xQueueReceive(command_queue,&frame,pdMS_TO_TICKS(20)) == pdTRUE)
            handle_command(&frame,false);
        if (xQueueReceive(overflow_queue,&frame,0) == pdTRUE) handle_command(&frame,true);
        LOCK(); state.control_progress = now_ms(); UNLOCK();
        esp_task_wdt_reset();
    }
}
static void add_sample(cJSON *j, state_t s) {
    cJSON_AddStringToObject(j,"sensor_id","temperature-1");
    cJSON_AddStringToObject(j,"metric","temperature");
    cJSON_AddStringToObject(j,"unit","degC");
    cJSON_AddNumberToObject(j,"sample_seq",(double)s.sample_seq);
    if (s.sample_seq) {
        cJSON_AddNumberToObject(j,"sample_uptime_ms",(double)s.sample_ms);
        cJSON_AddNumberToObject(j,"sample_age_ms",(double)(now_ms()-s.sample_ms));
        add_time(j,"sample_observed_at",s.sample_utc);
    } else {
        cJSON_AddNullToObject(j,"sample_uptime_ms");
        cJSON_AddNullToObject(j,"sample_age_ms");
        cJSON_AddNullToObject(j,"sample_observed_at");
    }
    if (s.sample_valid) cJSON_AddNumberToObject(j,"value",s.temperature);
    else {
        cJSON_AddNullToObject(j,"value");
        cJSON_AddStringToObject(j,"reason",s.sample_seq ? "sensor_disconnected_or_read_error" : "not_sampled");
    }
    cJSON_AddStringToObject(j,"quality",s.sample_valid ? "valid" : "invalid");
}
static bool publish_state(state_t s) {
    cJSON *j = envelope(stamp());
    if (!j) return false;
    cJSON_AddBoolToObject(j,"online",s.ready);
    cJSON_AddStringToObject(j,"lifecycle",s.config_error ? "CONFIG_ERROR" : s.ready ? "ONLINE" : "LOCAL_READY");
    add_output(j,s.output_on);
    cJSON_AddStringToObject(j,"requested_output",s.requested_on ? "ON" : "OFF");
    cJSON_AddStringToObject(j,"last_command_id",s.last_command);
    cJSON_AddStringToObject(j,"last_command_status",s.last_status);
    add_config(j,s.config);
    add_sample(j,s);
    cJSON_AddBoolToObject(j,"temperature_high_active",s.rules.high_active);
    cJSON_AddBoolToObject(j,"sensor_fault_active",s.rules.fault_active);
    cJSON_AddNumberToObject(j,"temperature_high_episode",s.rules.high_episode);
    cJSON_AddNumberToObject(j,"sensor_fault_episode",s.rules.fault_episode);
    if (s.have_valid) {
        cJSON_AddNumberToObject(j,"last_valid_value",s.last_valid);
        cJSON_AddNumberToObject(j,"last_valid_age_ms",(double)(now_ms()-s.last_valid_ms));
    }
    return publish_json("state",j,1,true);
}
static bool publish_heartbeat(state_t s) {
    cJSON *j = envelope(stamp());
    if (!j) return false;
    cJSON_AddStringToObject(j,"firmware_version",FW_VERSION);
    cJSON_AddStringToObject(j,"profile",
#ifdef CONFIG_BRIQON_DEMO
        "wokwi_demo"
#else
        "hardware_pilot"
#endif
    );
    cJSON_AddBoolToObject(j,"ready",s.ready);
    cJSON_AddNumberToObject(j,"config_version",s.config.version);
    cJSON_AddNumberToObject(j,"free_heap",esp_get_free_heap_size());
    cJSON_AddNumberToObject(j,"minimum_free_heap",esp_get_minimum_free_heap_size());
    cJSON *d = cJSON_AddObjectToObject(j,"diagnostics");
    cJSON_AddNumberToObject(d,"samples",s.sample_count);
    cJSON_AddNumberToObject(d,"sensor_errors",s.sensor_errors);
    cJSON_AddNumberToObject(d,"malformed",s.malformed);
    cJSON_AddNumberToObject(d,"busy",s.busy);
    cJSON_AddNumberToObject(d,"dropped_results",s.dropped_results);
    cJSON_AddNumberToObject(d,"dropped_notifications",s.dropped_notifications);
    cJSON_AddNumberToObject(d,"publish_failures",s.publish_failures);
    cJSON_AddNumberToObject(d,"coalesced_samples",s.coalesced);
    cJSON_AddNumberToObject(d,"mqtt_connections",s.reconnects);
    cJSON_AddNumberToObject(d,"outbox_bytes",esp_mqtt_client_get_outbox_size(mqtt));
    cJSON_AddNumberToObject(d,"command_queue_depth",uxQueueMessagesWaiting(command_queue));
    cJSON_AddNumberToObject(d,"notification_queue_depth",uxQueueMessagesWaiting(notification_queue));
    cJSON_AddNumberToObject(d,"acquisition_age_ms",(double)(now_ms()-s.acquisition_progress));
    cJSON_AddNumberToObject(d,"control_age_ms",(double)(now_ms()-s.control_progress));
    cJSON_AddNumberToObject(d,"publisher_age_ms",(double)(now_ms()-s.publish_progress));
    cJSON *stacks = cJSON_AddObjectToObject(j,"stack_free_bytes");
    cJSON_AddNumberToObject(stacks,"acquisition",uxTaskGetStackHighWaterMark(acquisition_handle));
    cJSON_AddNumberToObject(stacks,"control",uxTaskGetStackHighWaterMark(control_handle));
    cJSON_AddNumberToObject(stacks,"publisher",uxTaskGetStackHighWaterMark(publisher_handle));
    cJSON_AddNumberToObject(stacks,"supervisor",uxTaskGetStackHighWaterMark(supervisor_handle));
    return publish_json("heartbeat",j,0,false);
}
static bool cached_result_next(outbound_t *event, int *index, bool *accepted, uint64_t *order) {
    LOCK();
    for (int i=0; i<BQ_COMMAND_CACHE; ++i) {
        if (cache[i].accepted_pending || cache[i].terminal_pending) {
            *accepted = cache[i].accepted_pending;
            *event = *accepted ? cache[i].accepted : cache[i].terminal;
            *index = i; *order = cache[i].order;
            UNLOCK(); return true;
        }
    }
    UNLOCK(); return false;
}
static void publisher_task(void *unused) {
    (void)unused;
    task_started("publisher");
    uint64_t telemetry_due=0, heartbeat_due=0, state_due=0, last_sample=0;
    char announced_session[40] = "";
    outbound_t pending_rejection, pending_notification;
    bool has_rejection=false, has_notification=false;
    for (;;) {
        state_t s = snapshot();
        uint64_t now = now_ms();
        if (s.ready) {
            if (strcmp(announced_session,s.session)) {
                cJSON *j = envelope(stamp());
                cJSON_AddBoolToObject(j,"online",true);
                if (publish_json("availability",j,1,true) && publish_state(s)) {
                    strlcpy(announced_session,s.session,sizeof(announced_session));
                    heartbeat_due=0; telemetry_due=0;
                    ESP_LOGI(TAG,"ONLINE base=%s session=%s",base_topic,s.session);
                }
            }
            /* Bounded result work per pass prevents command floods starving snapshots. */
            for (int n=0; n<8; ++n) {
                outbound_t e; int index; bool accepted; uint64_t order;
                if (!cached_result_next(&e,&index,&accepted,&order)) break;
                if (!publish_json("command-results",event_json(&e),1,false)) break;
                LOCK();
                if (cache[index].order == order) {
                    if (accepted) cache[index].accepted_pending=false;
                    else cache[index].terminal_pending=false;
                }
                UNLOCK();
            }
            if (!has_rejection) has_rejection=xQueueReceive(rejection_queue,&pending_rejection,0)==pdTRUE;
            if (has_rejection && publish_json("command-results",event_json(&pending_rejection),1,false)) has_rejection=false;
            if (!has_notification) has_notification=xQueueReceive(notification_queue,&pending_notification,0)==pdTRUE;
            if (has_notification && publish_json("notifications",event_json(&pending_notification),1,false)) has_notification=false;
            if (now >= telemetry_due && s.sample_seq) {
                cJSON *j = envelope(stamp()); add_sample(j,s); add_output(j,s.output_on);
                if (publish_json("telemetry",j,0,false)) {
                    LOCK();
                    if (s.sample_seq > last_sample+1) state.coalesced += (uint32_t)(s.sample_seq-last_sample-1);
                    UNLOCK(); last_sample=s.sample_seq;
                    telemetry_due=now+s.config.telemetry_interval_ms;
                }
            }
            if (now >= heartbeat_due && publish_heartbeat(s)) heartbeat_due=now+10000;
            if (now >= state_due) { publish_state(s); state_due=now+1000; }
        }
        LOCK(); state.publish_progress=now_ms(); UNLOCK();
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
static uint32_t backoff(unsigned attempt) {
    unsigned shift = attempt > 5 ? 5 : attempt;
    uint32_t cap = 1000u << shift;
    if (cap > 30000) cap = 30000;
    /* 75-100% jitter, capped at 30 s. */
    return cap - cap/4 + esp_random() % (cap/4 + 1);
}
static esp_mqtt_client_config_t mqtt_config(void) {
    esp_mqtt_client_config_t c = {
        .broker.address.uri=CONFIG_BRIQON_BROKER_URI,
        .credentials.client_id=client_id,
        .credentials.username=CONFIG_BRIQON_MQTT_USERNAME,
        .credentials.authentication.password=CONFIG_BRIQON_MQTT_PASSWORD,
        .session.protocol_ver=MQTT_PROTOCOL_V_3_1_1,
        .session.disable_clean_session=false,
        .session.keepalive=20,
        .session.last_will.topic=NULL,
        .network.disable_auto_reconnect=true,
        .network.timeout_ms=10000,
        .buffer.size=1024,
        .buffer.out_size=2048,
        .task.stack_size=8192,
        .outbox.limit=BQ_OUTBOX_MAX,
    };
    static char availability_topic[192];
    snprintf(availability_topic,sizeof(availability_topic),"%s/availability",base_topic);
    c.session.last_will.topic=availability_topic;
    c.session.last_will.msg=will_json;
    c.session.last_will.qos=1;
    c.session.last_will.retain=1;
    if (!strncmp(CONFIG_BRIQON_BROKER_URI,"mqtts://",8))
        c.broker.verification.crt_bundle_attach=esp_crt_bundle_attach;
    return c;
}
static void prepare_session(void) {
    char session[40]; random_identity(session,sizeof(session),"session");
    LOCK();
    strlcpy(state.session,session,sizeof(state.session));
    state.subscription_id=-1;
    lost_readiness();
    UNLOCK();
    snprintf(will_json,sizeof(will_json),
        "{\"schema_version\":1,\"device_id\":\"%s\",\"boot_id\":\"%s\","
        "\"session_id\":\"%s\",\"online\":false}",device_id,boot_id,session);
}
static void supervisor_task(void *unused) {
    (void)unused;
    task_started("supervisor");
    bool started=false;
#ifdef CONFIG_BRIQON_DEMO
    bool network_fault=false;
#endif
    unsigned wifi_attempt=0, mqtt_attempt=0;
    uint64_t next_wifi=0, next_mqtt=0, wifi_deadline=0, attempt_deadline=0, diag_due=0;
    for (;;) {
        state_t s=snapshot(); uint64_t now=now_ms();
#ifdef CONFIG_BRIQON_DEMO
        bool inject_network=gpio_get_level(7)==0;
        if (inject_network && !network_fault) {
            esp_wifi_disconnect();
            LOCK();
            state.ip_ready=false; state.wifi_connected=false; state.wifi_connecting=false;
            state.mqtt_connected=false; lost_readiness();
            UNLOCK();
            ESP_LOGW(TAG,"test hook: Wi-Fi disconnected while GPIO7 LOW");
        }
        network_fault=inject_network;
        if (inject_network) {
            if (started) { esp_mqtt_client_stop(mqtt); started=false; }
            esp_task_wdt_reset(); vTaskDelay(pdMS_TO_TICKS(100)); continue;
        }
#endif
        if (!s.ip_ready) {
            if (started) { esp_mqtt_client_stop(mqtt); started=false; }
            if ((s.wifi_connecting || s.wifi_connected) && now >= wifi_deadline) {
                esp_wifi_disconnect();
                LOCK(); state.wifi_connected=false; state.wifi_connecting=false; UNLOCK();
                next_wifi=now+backoff(wifi_attempt++);
                ESP_LOGW(TAG,"Wi-Fi/IP attempt timed out; retry scheduled");
            } else if (!s.wifi_connecting && !s.wifi_connected && now >= next_wifi) {
                LOCK(); state.wifi_connecting=true; UNLOCK();
                esp_err_t err=esp_wifi_connect();
                if (err!=ESP_OK) { LOCK(); state.wifi_connecting=false; UNLOCK(); }
                ESP_LOGI(TAG,"Wi-Fi attempt=%u result=%s",wifi_attempt+1,esp_err_to_name(err));
                next_wifi=now+backoff(wifi_attempt++);
                wifi_deadline=now+15000;
            }
        } else {
            wifi_attempt=0;
            bool secure_time_ready=true;
#ifndef CONFIG_BRIQON_DEMO
            secure_time_ready=time(NULL)>=1704067200;
#endif
            if (s.ready) mqtt_attempt=0;
            else if (started && now >= attempt_deadline) {
                esp_mqtt_client_stop(mqtt); started=false;
                next_mqtt=now+backoff(mqtt_attempt++);
            } else if (!started && secure_time_ready && now >= next_mqtt) {
                prepare_session();
                esp_mqtt_client_config_t cfg=mqtt_config();
                esp_err_t err=esp_mqtt_set_config(mqtt,&cfg);
                if (err==ESP_OK) err=esp_mqtt_client_start(mqtt);
                started=err==ESP_OK;
                attempt_deadline=now+20000;
                next_mqtt=now+backoff(mqtt_attempt++);
                ESP_LOGI(TAG,"MQTT attempt started=%d",started);
            }
        }
        if (now >= diag_due) {
            ESP_LOGI(TAG,"health samples=%" PRIu32 " ready=%d heap=%" PRIu32
                " cmdq=%u notifq=%u malformed=%" PRIu32 " busy=%" PRIu32,
                s.sample_count,s.ready,esp_get_free_heap_size(),
                (unsigned)uxQueueMessagesWaiting(command_queue),
                (unsigned)uxQueueMessagesWaiting(notification_queue),s.malformed,s.busy);
            diag_due=now+10000;
        }
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
static bool topic_identifier(const char *s) {
    size_t len=strlen(s);
    if (!len || len>64) return false;
    for (size_t i=0; i<len; ++i)
        if (!((s[i]>='a'&&s[i]<='z') || (s[i]>='A'&&s[i]<='Z') ||
              (s[i]>='0'&&s[i]<='9') || s[i]=='-' || s[i]=='_')) return false;
    return true;
}
static void provision_identity(const uint8_t mac[6]) {
    if (strlen(CONFIG_BRIQON_DEVICE_ID)) {
        strlcpy(device_id,CONFIG_BRIQON_DEVICE_ID,sizeof(device_id)); return;
    }
    nvs_handle_t nvs;
    esp_err_t err=nvs_open("briqon",NVS_READWRITE,&nvs);
    if (err==ESP_OK) {
        size_t size=sizeof(device_id);
        esp_err_t get=nvs_get_str(nvs,"device_id",device_id,&size);
        if (get==ESP_OK) { nvs_close(nvs); return; }
        if (get!=ESP_ERR_NVS_NOT_FOUND) {
            state.config_error=true; nvs_close(nvs);
            snprintf(device_id,sizeof(device_id),"briqon-invalid-%02x%02x%02x",mac[3],mac[4],mac[5]);
            return;
        }
        snprintf(device_id,sizeof(device_id),"briqon-demo-%02x%02x%02x-%08" PRIx32,
                 mac[3],mac[4],mac[5],esp_random());
        if (state.config_error || nvs_set_str(nvs,"device_id",device_id)!=ESP_OK || nvs_commit(nvs)!=ESP_OK)
            state.config_error=true;
        nvs_close(nvs);
    } else {
        state.config_error=true;
        snprintf(device_id,sizeof(device_id),"briqon-invalid-%02x%02x%02x",mac[3],mac[4],mac[5]);
    }
}
void app_main(void) {
    /* Establish OFF before NVS, task creation, or networking. */
#ifdef CONFIG_BRIQON_RELAY_ACTIVE_LOW
    gpio_set_level(RELAY_GPIO,1);
#else
    gpio_set_level(RELAY_GPIO,0);
#endif
    gpio_config_t relay={.pin_bit_mask=1ULL<<RELAY_GPIO, .mode=GPIO_MODE_OUTPUT};
    ESP_ERROR_CHECK(gpio_config(&relay));
#ifdef CONFIG_BRIQON_DEMO
    gpio_config_t network_button={.pin_bit_mask=1ULL<<7, .mode=GPIO_MODE_INPUT,
        .pull_up_en=GPIO_PULLUP_ENABLE};
    ESP_ERROR_CHECK(gpio_config(&network_button));
#endif
    state_lock=xSemaphoreCreateMutex(); assert(state_lock);
    load_config();
    uint8_t mac[6]; ESP_ERROR_CHECK(esp_read_mac(mac,ESP_MAC_WIFI_STA));
    provision_identity(mac);
    random_identity(boot_id,sizeof(boot_id),"boot");
    if (!topic_identifier(device_id) || !topic_identifier(CONFIG_BRIQON_ENVIRONMENT)) {
        ESP_LOGE(TAG,"invalid device/environment identifier; output OFF"); return;
    }
    snprintf(base_topic,sizeof(base_topic),"briqon/v1/%s/%s",CONFIG_BRIQON_ENVIRONMENT,device_id);
    snprintf(command_topic,sizeof(command_topic),"%s/commands",base_topic);
    snprintf(client_id,sizeof(client_id),"%s-%s",device_id,boot_id);
    ESP_LOGI(TAG,"BRIQON firmware=%s idf=%s reset=%d device=%s boot=%s",FW_VERSION,
        esp_get_idf_version(),(int)esp_reset_reason(),device_id,boot_id);
    ESP_LOGI(TAG,"topics=%s/# config_version=%" PRIu32 " config_error=%d",base_topic,state.config.version,state.config_error);
#ifdef CONFIG_BRIQON_DEMO
    ESP_LOGW(TAG,"wokwi_demo: disposable test namespace; GPIO6 LOW injects sensor fault");
#else
    if (strncmp(CONFIG_BRIQON_BROKER_URI,"mqtts://",8) ||
        !strlen(CONFIG_BRIQON_MQTT_USERNAME) || !strlen(CONFIG_BRIQON_MQTT_PASSWORD)) {
        ESP_LOGE(TAG,"hardware profile needs mqtts:// and credentials; output OFF"); return;
    }
#endif
    command_queue=xQueueCreate(BQ_COMMAND_QUEUE,sizeof(inbound_t));
    overflow_queue=xQueueCreate(4,sizeof(inbound_t));
    rejection_queue=xQueueCreate(32,sizeof(outbound_t));
    notification_queue=xQueueCreate(32,sizeof(outbound_t));
    assert(command_queue && overflow_queue && rejection_queue && notification_queue);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    assert(esp_netif_create_default_wifi_sta());
    wifi_init_config_t wifi_init=WIFI_INIT_CONFIG_DEFAULT();
    wifi_init.nvs_enable=0; /* provisioning comes from config; no Wi-Fi NVS writes */
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,ESP_EVENT_ANY_ID,wifi_event,NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,wifi_event,NULL));
    wifi_config_t wifi={0};
    strlcpy((char *)wifi.sta.ssid,CONFIG_BRIQON_WIFI_SSID,sizeof(wifi.sta.ssid));
    strlcpy((char *)wifi.sta.password,CONFIG_BRIQON_WIFI_PASSWORD,sizeof(wifi.sta.password));
    wifi.sta.scan_method=WIFI_ALL_CHANNEL_SCAN;
    wifi.sta.threshold.authmode=strlen(CONFIG_BRIQON_WIFI_PASSWORD) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA,&wifi));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_sntp_config_t ntp=ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    ESP_ERROR_CHECK(esp_netif_sntp_init(&ntp));
    prepare_session();
    esp_mqtt_client_config_t cfg=mqtt_config();
    mqtt=esp_mqtt_client_init(&cfg); assert(mqtt);
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(mqtt,ESP_EVENT_ANY_ID,mqtt_event,NULL));
    assert(xTaskCreatePinnedToCore(control_task,"control",8192,NULL,6,&control_handle,1)==pdPASS);
    assert(xTaskCreatePinnedToCore(acquisition_task,"acquisition",6144,NULL,4,&acquisition_handle,1)==pdPASS);
    assert(xTaskCreate(publisher_task,"publisher",8192,NULL,3,&publisher_handle)==pdPASS);
    assert(xTaskCreate(supervisor_task,"supervisor",6144,NULL,3,&supervisor_handle)==pdPASS);
}
