#include "ds18b20_frame.h"
#include <stdbool.h>

uint8_t bq_onewire_crc8(const uint8_t *data, size_t length) {
    uint8_t crc=0;
    for (size_t i=0; i<length; ++i) {
        uint8_t byte=data[i];
        for (unsigned bit=0; bit<8; ++bit) {
            uint8_t mix=(crc ^ byte) & 1;
            crc >>= 1;
            if (mix) crc ^= 0x8c;
            byte >>= 1;
        }
    }
    return crc;
}
bq_ds_status_t bq_ds18b20_decode(const uint8_t scratch[9], float *temperature) {
    bool all_zero=true, all_one=true;
    for (unsigned i=0; i<9; ++i) {
        if (scratch[i]!=0) all_zero=false;
        if (scratch[i]!=0xff) all_one=false;
    }
    if (all_zero || all_one) return BQ_DS_EMPTY;
    if (bq_onewire_crc8(scratch,8)!=scratch[8]) return BQ_DS_CRC_ERROR;
    uint16_t raw=(uint16_t)scratch[0] | (uint16_t)scratch[1]<<8;
    unsigned resolution=(scratch[4]>>5)&3;
    static const uint16_t masks[]={0xfff8,0xfffc,0xfffe,0xffff};
    raw &= masks[resolution];
    int32_t signed_raw=raw & 0x8000 ? (int32_t)raw-65536 : raw;
    float value=signed_raw/16.0f;
    if (value < -55 || value > 125) return BQ_DS_RANGE_ERROR;
    *temperature=value;
    return BQ_DS_OK;
}
