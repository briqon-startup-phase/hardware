#include "sensor_adapter.h"
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include <math.h>
#include <stdbool.h>

#define DATA_GPIO 4
static bool initialized;

#ifdef CONFIG_BRIQON_DEMO
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"
#include "ds18b20_frame.h"

/* Genuine single-device 1-Wire over GPIO for the simulator profile. Only wire
   slots are critical; the 800 ms conversion wait yields to the control task. */
static portMUX_TYPE wire_lock=portMUX_INITIALIZER_UNLOCKED;
static bool wire_reset(void) {
    portENTER_CRITICAL(&wire_lock);
    gpio_set_level(DATA_GPIO,1);
    esp_rom_delay_us(5);
    bool idle_high=gpio_get_level(DATA_GPIO)!=0;
    gpio_set_level(DATA_GPIO,0);
    esp_rom_delay_us(480);
    gpio_set_level(DATA_GPIO,1);
    esp_rom_delay_us(70);
    bool presence=gpio_get_level(DATA_GPIO)==0;
    esp_rom_delay_us(410);
    bool released=gpio_get_level(DATA_GPIO)!=0;
    portEXIT_CRITICAL(&wire_lock);
    return idle_high && presence && released;
}
static void wire_write_bit(bool bit) {
    portENTER_CRITICAL(&wire_lock);
    gpio_set_level(DATA_GPIO,0);
    esp_rom_delay_us(bit ? 6 : 60);
    gpio_set_level(DATA_GPIO,1);
    esp_rom_delay_us(bit ? 64 : 10);
    portEXIT_CRITICAL(&wire_lock);
}
static bool wire_read_bit(void) {
    portENTER_CRITICAL(&wire_lock);
    gpio_set_level(DATA_GPIO,0);
    esp_rom_delay_us(3);
    gpio_set_level(DATA_GPIO,1);
    esp_rom_delay_us(10);
    bool bit=gpio_get_level(DATA_GPIO)!=0;
    esp_rom_delay_us(53);
    portEXIT_CRITICAL(&wire_lock);
    return bit;
}
static void wire_write_byte(uint8_t byte) {
    for (unsigned bit=0; bit<8; ++bit) {
        wire_write_bit((byte & 1)!=0);
        byte >>= 1;
    }
}
static uint8_t wire_read_byte(void) {
    uint8_t byte=0;
    for (unsigned bit=0; bit<8; ++bit)
        if (wire_read_bit()) byte |= 1u<<bit;
    return byte;
}
static esp_err_t demo_read(float *temperature) {
    if (!wire_reset()) return ESP_ERR_NOT_FOUND;
    wire_write_byte(0xcc); /* Skip ROM: fixture has exactly one DS18B20. */
    wire_write_byte(0x44); /* Convert T. */
    vTaskDelay(pdMS_TO_TICKS(800));
    if (!wire_read_bit()) return ESP_ERR_TIMEOUT;
    if (!wire_reset()) return ESP_ERR_NOT_FOUND;
    wire_write_byte(0xcc);
    wire_write_byte(0xbe); /* Read scratchpad, including CRC byte. */
    uint8_t scratch[9];
    for (unsigned i=0; i<9; ++i) scratch[i]=wire_read_byte();
    switch (bq_ds18b20_decode(scratch,temperature)) {
    case BQ_DS_OK: return ESP_OK;
    case BQ_DS_CRC_ERROR: return ESP_ERR_INVALID_CRC;
    default: return ESP_ERR_INVALID_RESPONSE;
    }
}
#else
#include "onewire_bus.h"
#include "ds18b20.h"
static onewire_bus_handle_t bus;
static ds18b20_device_handle_t sensor;
#endif

esp_err_t bq_sensor_init(void) {
    if (initialized) return ESP_OK;
#ifdef CONFIG_BRIQON_DEMO
    gpio_config_t fault={.pin_bit_mask=1ULL<<6, .mode=GPIO_MODE_INPUT,
        .pull_up_en=GPIO_PULLUP_ENABLE};
    esp_err_t err=gpio_config(&fault);
    if (err!=ESP_OK) return err;
    gpio_set_level(DATA_GPIO,1);
    gpio_config_t data={.pin_bit_mask=1ULL<<DATA_GPIO, .mode=GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en=GPIO_PULLUP_ENABLE};
    err=gpio_config(&data);
    if (err!=ESP_OK) return err;
    gpio_set_level(DATA_GPIO,1);
    ESP_LOGI("briqon.sensor","backend=gpio-onewire gpio=%d (genuine sensor + CRC)",DATA_GPIO);
#else
    if (!bus) {
        onewire_bus_config_t cfg={.bus_gpio_num=DATA_GPIO};
        onewire_bus_rmt_config_t rmt={.max_rx_bytes=10};
        esp_err_t err=onewire_new_bus_rmt(&cfg,&rmt,&bus);
        if (err!=ESP_OK) return err;
    }
    if (!sensor) {
        ds18b20_config_t cfg={};
        esp_err_t err=ds18b20_new_device_from_bus(bus,&cfg,&sensor);
        if (err!=ESP_OK) return err;
    }
    ESP_LOGI("briqon.sensor","backend=espressif-rmt gpio=%d",DATA_GPIO);
#endif
    initialized=true;
    return ESP_OK;
}
esp_err_t bq_sensor_read(float *temperature) {
    if (!temperature) return ESP_ERR_INVALID_ARG;
    if (!initialized) {
        esp_err_t err=bq_sensor_init();
        if (err!=ESP_OK) return err;
    }
#ifdef CONFIG_BRIQON_DEMO
    if (gpio_get_level(6)==0) return ESP_ERR_INVALID_STATE;
    esp_err_t err=demo_read(temperature);
#else
    esp_err_t err=ds18b20_trigger_temperature_conversion(sensor);
    if (err==ESP_OK) err=ds18b20_get_temperature(sensor,temperature);
#endif
    if (err==ESP_OK && (!isfinite(*temperature) || *temperature < -55 || *temperature > 125))
        err=ESP_ERR_INVALID_RESPONSE;
    return err;
}
