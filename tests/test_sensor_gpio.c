/* Run the actual sensor adapter against a deterministic host 1-Wire model.
   This checks protocol/CRC/error handling, not Wokwi peripheral compatibility. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "sensor_adapter.h"
#include "ds18b20_frame.h"
#include "driver/gpio.h"

static uint64_t clock_us, low_since, presence_from, presence_to, read_low_to, conversion_ready;
static uint64_t critical_since, max_critical_us;
static bool drive_low, present=true, sensor_fault, bus_short, slow_conversion;
static unsigned phase, byte_in, bit_in, read_bit, critical, resets, conversions;
static uint8_t scratch[9]={0x50,0x05,0x4b,0x46,0x7f,0xff,0x0c,0x10,0x1c};
enum { EXPECT_ROM, EXPECT_COMMAND, CONVERTING, SCRATCHPAD };

void bq_test_enter(void) { assert(!critical); critical=1; critical_since=clock_us; }
void bq_test_exit(void) {
    assert(critical==1); critical=0;
    uint64_t elapsed=clock_us-critical_since;
    if (elapsed>max_critical_us) max_critical_us=elapsed;
}
void esp_rom_delay_us(uint32_t us) { clock_us+=us; }
void vTaskDelay(uint32_t ticks) { assert(!critical); clock_us+=(uint64_t)ticks*1000; }
esp_err_t gpio_config(const gpio_config_t *cfg) {
    assert(cfg->pull_up_en==GPIO_PULLUP_ENABLE);
    if (cfg->pin_bit_mask==(1ULL<<4)) assert(cfg->mode==GPIO_MODE_INPUT_OUTPUT_OD);
    return ESP_OK;
}
static void command_bit(bool bit) {
    byte_in|=(unsigned)bit<<bit_in;
    if (++bit_in!=8) return;
    if (phase==EXPECT_ROM) { assert(byte_in==0xcc); phase=EXPECT_COMMAND; }
    else {
        assert(phase==EXPECT_COMMAND);
        if (byte_in==0x44) {
            phase=CONVERTING; conversions++;
            conversion_ready=clock_us+(slow_conversion ? 2000000 : 750000);
        } else { assert(byte_in==0xbe); phase=SCRATCHPAD; read_bit=0; }
    }
    byte_in=0; bit_in=0;
}
esp_err_t gpio_set_level(int pin, uint32_t level) {
    assert(pin==4);
    if (!level) { assert(!drive_low); drive_low=true; low_since=clock_us; return ESP_OK; }
    if (!drive_low) return ESP_OK;
    drive_low=false;
    uint64_t duration=clock_us-low_since;
    if (duration>=480) {
        assert(duration<=960);
        resets++; phase=EXPECT_ROM; byte_in=0; bit_in=0;
        presence_from=clock_us+20; presence_to=clock_us+200; read_low_to=0;
    } else if (phase==CONVERTING || phase==SCRATCHPAD) {
        assert(duration<=15);
        bool response;
        if (phase==CONVERTING) response=clock_us>=conversion_ready;
        else { assert(read_bit<72); response=(scratch[read_bit/8]>>(read_bit%8))&1; read_bit++; }
        read_low_to=response ? 0 : low_since+50;
    } else {
        assert(duration<=15 || (duration>=60 && duration<=120));
        command_bit(duration<=15);
    }
    return ESP_OK;
}
int gpio_get_level(int pin) {
    if (pin==6) return !sensor_fault;
    assert(pin==4);
    if (drive_low || bus_short) return 0;
    if (!present) return 1;
    if (clock_us>=presence_from && clock_us<presence_to) return 0;
    if (clock_us<read_low_to) return 0;
    return 1;
}
static void set_temperature(int16_t raw, uint8_t config) {
    scratch[0]=(uint16_t)raw&0xff; scratch[1]=(uint16_t)raw>>8;
    scratch[4]=config; scratch[8]=bq_onewire_crc8(scratch,8);
}
int main(void) {
    unsigned checks=0;
#define CHECK(expr) do { checks++; assert(expr); } while (0)
    CHECK(bq_onewire_crc8(scratch,8)==0x1c); /* Datasheet power-on scratchpad. */
    CHECK(bq_sensor_init()==ESP_OK);
    float temperature=-999;
    CHECK(bq_sensor_read(&temperature)==ESP_OK && temperature==85.0f);
    CHECK(resets==2 && conversions==1 && read_bit==72);
    set_temperature(64,0x7f);
    CHECK(bq_sensor_read(&temperature)==ESP_OK && temperature==4.0f);
    set_temperature(-162,0x7f);
    CHECK(bq_sensor_read(&temperature)==ESP_OK && temperature==-10.125f);
    set_temperature(-880,0x7f);
    CHECK(bq_sensor_read(&temperature)==ESP_OK && temperature==-55.0f);
    set_temperature(2000,0x7f);
    CHECK(bq_sensor_read(&temperature)==ESP_OK && temperature==125.0f);
    set_temperature(65,0x1f);
    CHECK(bq_sensor_read(&temperature)==ESP_OK && temperature==4.0f); /* 9 bit masks undefined bits. */
    set_temperature(64,0x7f); scratch[8]^=1; temperature=-999;
    CHECK(bq_sensor_read(&temperature)==ESP_ERR_INVALID_CRC && temperature==-999);
    memset(scratch,0,sizeof(scratch));
    CHECK(bq_sensor_read(&temperature)==ESP_ERR_INVALID_RESPONSE && temperature==-999);
    memset(scratch,0xff,sizeof(scratch));
    CHECK(bq_sensor_read(&temperature)==ESP_ERR_INVALID_RESPONSE && temperature==-999);
    present=false;
    CHECK(bq_sensor_read(&temperature)==ESP_ERR_NOT_FOUND && temperature==-999);
    present=true; bus_short=true;
    CHECK(bq_sensor_read(&temperature)==ESP_ERR_NOT_FOUND && temperature==-999);
    bus_short=false; sensor_fault=true;
    unsigned before=resets;
    CHECK(bq_sensor_read(&temperature)==ESP_ERR_INVALID_STATE && resets==before);
    sensor_fault=false; slow_conversion=true;
    CHECK(bq_sensor_read(&temperature)==ESP_ERR_TIMEOUT && temperature==-999);
    slow_conversion=false; set_temperature(2016,0x7f);
    CHECK(bq_sensor_read(&temperature)==ESP_ERR_INVALID_RESPONSE && temperature==-999);
    set_temperature(80,0x7f);
    CHECK(bq_sensor_read(&temperature)==ESP_OK && temperature==5.0f);
    CHECK(bq_sensor_read(NULL)==ESP_ERR_INVALID_ARG);
    CHECK(max_critical_us<1000 && !critical);
    printf("PASS: %u sensor checks (actual GPIO adapter + host bus model, CRC, faults, yielding)\n",checks);
    return 0;
}
