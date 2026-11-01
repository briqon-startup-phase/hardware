#include "contract.h"
#include "cJSON.h"
#include <string.h>
#include <math.h>
#include <ctype.h>

static bool keys(const cJSON *obj, const char *const *allowed, size_t n) {
    if (!cJSON_IsObject(obj)) return false;
    for (const cJSON *p = obj->child; p; p = p->next) {
        bool found = false;
        if (!p->string) return false;
        for (size_t i = 0; i < n; ++i)
            if (!strcmp(p->string, allowed[i])) found = true;
        if (!found) return false;
        for (const cJSON *q = p->next; q; q = q->next)
            if (q->string && !strcmp(p->string, q->string)) return false;
    }
    return true;
}
static const cJSON *item(const cJSON *o, const char *key) {
    return cJSON_GetObjectItemCaseSensitive(o, key);
}
static bool integer(const cJSON *v, double lo, double hi) {
    return cJSON_IsNumber(v) && isfinite(v->valuedouble) &&
        v->valuedouble >= lo && v->valuedouble <= hi &&
        floor(v->valuedouble) == v->valuedouble;
}
static bool identifier(const cJSON *v, char *dst, size_t size) {
    if (!cJSON_IsString(v) || !v->valuestring) return false;
    size_t n = strlen(v->valuestring);
    if (!n || n >= size) return false;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)v->valuestring[i];
        if (!isalnum(c) && c != '-' && c != '_' && c != '.' && c != ':') return false;
    }
    memcpy(dst, v->valuestring, n + 1);
    return true;
}
/* Cap parser recursion before cJSON touches attacker-controlled nesting. */
static bool shallow_json(const char *s, size_t n) {
    unsigned depth = 0;
    bool quoted = false, escape = false;
    for (size_t i = 0; i < n; ++i) {
        char c = s[i];
        if (c == '\0') return false;
        if (quoted) {
            if (escape) {
                /* This protocol has only ASCII field names/identifiers and numbers.
                   Reject unicode escapes, including hidden NUL/key aliases. */
                if (c == 'u') return false;
                escape = false;
            } else if (c == '\\') escape = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') quoted = true;
        else if (c == '{' || c == '[') { if (++depth > 8) return false; }
        else if (c == '}' || c == ']') { if (!depth) return false; --depth; }
    }
    return !quoted && depth == 0;
}
const char *bq_parse_command(const char *json, size_t length, bq_command_t *out) {
    memset(out, 0, sizeof(*out));
    if (!length || length > BQ_PAYLOAD_MAX || !shallow_json(json, length))
        return "invalid_json_or_size";
    cJSON *root = cJSON_ParseWithLengthOpts(json, length + 1, NULL, true);
    if (!root) return "invalid_json";
    const char *error = "invalid_schema";
    const char *const top[] = {"schema_version", "device_id", "target_boot_id",
        "target_session_id", "command_id", "expires_at_uptime_ms", "action", "params"};
    if (!keys(root, top, 8) || cJSON_GetArraySize(root) != 8 ||
        !integer(item(root, "schema_version"), 1, 1) ||
        !identifier(item(root,"command_id"), out->command_id, sizeof(out->command_id)) ||
        !identifier(item(root,"device_id"), out->device_id, sizeof(out->device_id)) ||
        !identifier(item(root,"target_boot_id"), out->boot_id, sizeof(out->boot_id)) ||
        !identifier(item(root,"target_session_id"), out->session_id, sizeof(out->session_id)) ||
        !integer(item(root,"expires_at_uptime_ms"), 0, 9007199254740991.0)) goto done;
    out->expires_ms = (uint64_t)item(root,"expires_at_uptime_ms")->valuedouble;
    const cJSON *a = item(root,"action"), *p = item(root,"params");
    if (!cJSON_IsString(a) || !cJSON_IsObject(p)) goto done;
    error = "invalid_params";
    if (!strcmp(a->valuestring,"get_status")) {
        out->action = BQ_GET_STATUS;
        if (p->child) goto done;
    } else if (!strcmp(a->valuestring,"set_output")) {
        out->action = BQ_SET_OUTPUT;
        const char *const names[] = {"channel", "state"};
        const cJSON *channel = item(p,"channel"), *state = item(p,"state");
        if (!keys(p,names,2) || cJSON_GetArraySize(p) != 2 ||
            !cJSON_IsString(channel) || strcmp(channel->valuestring,"relay-1") ||
            !cJSON_IsString(state)) goto done;
        if (!strcmp(state->valuestring,"ON")) out->output_on = true;
        else if (strcmp(state->valuestring,"OFF")) goto done;
    } else if (!strcmp(a->valuestring,"set_config")) {
        out->action = BQ_SET_CONFIG;
        const char *const names[] = {"expected_config_version", "sample_interval_ms",
            "telemetry_interval_ms", "high_c", "clear_c", "dwell_ms"};
        if (!keys(p,names,6) || !integer(item(p,names[0]),1,UINT32_MAX)) goto done;
        out->expected_version = (uint32_t)item(p,names[0])->valuedouble;
        const cJSON *v;
        if ((v = item(p,"sample_interval_ms"))) {
            if (!integer(v,1000,60000)) goto done;
            out->config_mask |= BQ_CFG_SAMPLE;
            out->config.sample_interval_ms = (uint32_t)v->valuedouble;
        }
        if ((v = item(p,"telemetry_interval_ms"))) {
            if (!integer(v,1000,60000)) goto done;
            out->config_mask |= BQ_CFG_TELEMETRY;
            out->config.telemetry_interval_ms = (uint32_t)v->valuedouble;
        }
        if ((v = item(p,"dwell_ms"))) {
            if (!integer(v,1000,60000)) goto done;
            out->config_mask |= BQ_CFG_DWELL;
            out->config.dwell_ms = (uint32_t)v->valuedouble;
        }
        if ((v = item(p,"high_c"))) {
            if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < -50 || v->valuedouble > 100) goto done;
            out->config_mask |= BQ_CFG_HIGH;
            out->config.high_c = (float)v->valuedouble;
        }
        if ((v = item(p,"clear_c"))) {
            if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < -50 || v->valuedouble > 100) goto done;
            out->config_mask |= BQ_CFG_CLEAR;
            out->config.clear_c = (float)v->valuedouble;
        }
        if (!out->config_mask) goto done;
    } else { error = "unsupported_action"; goto done; }
    error = NULL;
done:
    cJSON_Delete(root);
    return error;
}
int bq_assemble(bq_assembly_t *a, const void *data, int length, int offset,
                int total, bool topic_matches, bool retained, uint64_t now) {
    if (offset == 0) {
        memset(a, 0, sizeof(*a));
        if (!topic_matches || total <= 0 || total > BQ_PAYLOAD_MAX) return -1;
        a->active = true;
        a->total = (size_t)total;
        a->retained = retained;
        a->started_ms = now;
    }
    if (!a->active || !data || length <= 0 || offset < 0 ||
        (size_t)offset != a->used || total != (int)a->total ||
        (size_t)length > a->total - a->used || now - a->started_ms > 2000) {
        a->active = false;
        return -1;
    }
    memcpy(a->payload + a->used, data, (size_t)length);
    a->used += (size_t)length;
    if (a->used != a->total) return 0;
    a->payload[a->used] = '\0';
    a->active = false;
    return 1;
}
