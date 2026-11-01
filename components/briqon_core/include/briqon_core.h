#pragma once
#include <stdbool.h>
#include <stdint.h>

#define BQ_PAYLOAD_MAX 2048
#define BQ_COMMAND_CACHE 64
#define BQ_COMMAND_QUEUE 8
#define BQ_OUTBOX_MAX 16384
#define BQ_ID_MAX 64

typedef struct {
    uint32_t version, sample_interval_ms, telemetry_interval_ms, dwell_ms;
    float high_c, clear_c;
} bq_config_t;

typedef struct {
    bool high_active, fault_active, dwell_pending;
    uint64_t dwell_since;
    uint32_t config_version, high_episode, fault_episode;
    unsigned failures, successes;
} bq_rules_t;

enum { BQ_HIGH_OPEN = 1, BQ_HIGH_CLEAR = 2,
       BQ_FAULT_OPEN = 4, BQ_FAULT_CLEAR = 8 };

typedef enum { BQ_UNKNOWN, BQ_GET_STATUS, BQ_SET_OUTPUT, BQ_SET_CONFIG } bq_action_t;
enum { BQ_CFG_SAMPLE = 1, BQ_CFG_TELEMETRY = 2, BQ_CFG_HIGH = 4,
       BQ_CFG_CLEAR = 8, BQ_CFG_DWELL = 16 };

typedef struct {
    char command_id[BQ_ID_MAX + 1], boot_id[40], session_id[40], device_id[65];
    uint64_t expires_ms;
    bq_action_t action;
    bool output_on;
    uint32_t config_mask, expected_version;
    bq_config_t config;
} bq_command_t;

bq_config_t bq_default_config(void);
bool bq_config_valid(const bq_config_t *c);
bool bq_config_patch(const bq_config_t *old, const bq_command_t *cmd,
                     bq_config_t *next);
bool bq_deadline_valid(uint64_t deadline, uint64_t now);
unsigned bq_rules_sample(bq_rules_t *r, const bq_config_t *c,
                         bool valid, float temperature, uint64_t now);
const char *bq_action_name(bq_action_t action);
