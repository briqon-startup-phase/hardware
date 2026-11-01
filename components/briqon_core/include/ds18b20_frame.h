#pragma once
#include <stdint.h>
#include <stddef.h>

typedef enum {
    BQ_DS_OK, BQ_DS_EMPTY, BQ_DS_CRC_ERROR, BQ_DS_RANGE_ERROR
} bq_ds_status_t;

uint8_t bq_onewire_crc8(const uint8_t *data, size_t length);
/* Validate actual nine-byte scratchpad before modifying the output temperature. */
bq_ds_status_t bq_ds18b20_decode(const uint8_t scratch[9], float *temperature);
