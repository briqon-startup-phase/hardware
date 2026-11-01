#pragma once
#include "briqon_core.h"
#include <stddef.h>

/* Input must have length+1 bytes, with a trailing NUL. Embedded NUL is rejected. */
const char *bq_parse_command(const char *json, size_t length, bq_command_t *out);

typedef struct {
    char payload[BQ_PAYLOAD_MAX + 1];
    size_t used, total;
    uint64_t started_ms;
    bool active, retained;
} bq_assembly_t;
/* -1 = rejected, 0 = incomplete, 1 = complete. First fragment must match topic. */
int bq_assemble(bq_assembly_t *a, const void *data, int length, int offset,
                int total, bool topic_matches, bool retained, uint64_t now);
