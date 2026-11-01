#include "briqon_core.h"
#include <math.h>
#include <limits.h>

bq_config_t bq_default_config(void) {
    return (bq_config_t){.version=1, .sample_interval_ms=1000,
        .telemetry_interval_ms=2000, .dwell_ms=5000, .high_c=8, .clear_c=6};
}
bool bq_config_valid(const bq_config_t *c) {
    return c->version > 0 && c->sample_interval_ms >= 1000 &&
        c->sample_interval_ms <= 60000 && c->telemetry_interval_ms >= 1000 &&
        c->telemetry_interval_ms <= 60000 && c->dwell_ms >= 1000 &&
        c->dwell_ms <= 60000 && isfinite(c->high_c) && isfinite(c->clear_c) &&
        c->high_c >= -50 && c->high_c <= 100 && c->clear_c >= -50 &&
        c->clear_c <= 100 && c->clear_c < c->high_c;
}
bool bq_config_patch(const bq_config_t *old, const bq_command_t *cmd,
                     bq_config_t *next) {
    if (old->version != cmd->expected_version || old->version == UINT32_MAX ||
        !cmd->config_mask || (cmd->config_mask & ~31u)) return false;
    *next = *old;
    next->version++;
    if (cmd->config_mask & BQ_CFG_SAMPLE) next->sample_interval_ms = cmd->config.sample_interval_ms;
    if (cmd->config_mask & BQ_CFG_TELEMETRY) next->telemetry_interval_ms = cmd->config.telemetry_interval_ms;
    if (cmd->config_mask & BQ_CFG_HIGH) next->high_c = cmd->config.high_c;
    if (cmd->config_mask & BQ_CFG_CLEAR) next->clear_c = cmd->config.clear_c;
    if (cmd->config_mask & BQ_CFG_DWELL) next->dwell_ms = cmd->config.dwell_ms;
    return bq_config_valid(next);
}
bool bq_deadline_valid(uint64_t deadline, uint64_t now) {
    return deadline > now && deadline - now <= 30000;
}
unsigned bq_rules_sample(bq_rules_t *r, const bq_config_t *c,
                         bool valid, float t, uint64_t now) {
    unsigned changes = 0;
    if (r->config_version != c->version) {
        r->dwell_pending = false;
        r->config_version = c->version;
    }
    if (!valid || !isfinite(t)) {
        r->dwell_pending = false;
        r->successes = 0;
        if (r->failures < 3) r->failures++;
        if (r->failures == 3 && !r->fault_active) {
            r->fault_active = true;
            r->fault_episode++;
            changes |= BQ_FAULT_OPEN;
        }
        return changes;
    }
    r->failures = 0;
    if (r->successes < 3) r->successes++;
    if (r->successes == 3 && r->fault_active) {
        r->fault_active = false;
        changes |= BQ_FAULT_CLEAR;
    }
    bool condition = r->high_active ? t <= c->clear_c : t > c->high_c;
    if (!condition) r->dwell_pending = false;
    else if (!r->dwell_pending) {
        r->dwell_pending = true;
        r->dwell_since = now;
    } else if (now - r->dwell_since >= c->dwell_ms) {
        r->dwell_pending = false;
        r->high_active = !r->high_active;
        if (r->high_active) { r->high_episode++; changes |= BQ_HIGH_OPEN; }
        else changes |= BQ_HIGH_CLEAR;
    }
    return changes;
}
const char *bq_action_name(bq_action_t a) {
    switch (a) {
    case BQ_GET_STATUS: return "get_status";
    case BQ_SET_OUTPUT: return "set_output";
    case BQ_SET_CONFIG: return "set_config";
    default: return "unknown";
    }
}
